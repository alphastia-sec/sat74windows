#include "PluginProcessor.h"
#include "PluginEditor.h"

// ════════════════════════════════════════════════════════════════════════════
//  Layout parametrów - input/output od -inf (mute) do +24 dB z skewem na 0 dB
// ════════════════════════════════════════════════════════════════════════════
juce::AudioProcessorValueTreeState::ParameterLayout ABI1176Processor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterBool>("bypass", "Bypass", false));

    // Input / Output: od "absolutnej ciszy" (-100 dB, traktowane jako mute) do +24 dB.
    // Skew = 0.5 (skewFromMidpoint = false) -> więcej rozdzielczości przy 0 dB.
    // Punkt środkowy slidera (50 %) ≈ 0 dB -> wygodna obsługa knobami.
    juce::NormalisableRange<float> gainRange (-100.0f, 24.0f, 0.1f);
    gainRange.setSkewForCentre (0.0f);   // 0 dB ląduje na środku slidera

    params.push_back(std::make_unique<juce::AudioParameterFloat>("inputGain", "Input Gain",
        gainRange, 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("dB")
            .withStringFromValueFunction ([](float v, int) {
                return v <= -99.5f ? juce::String ("-inf")
                                   : juce::String (v, 1);
            })));

    params.push_back(std::make_unique<juce::AudioParameterFloat>("drive", "Drive",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>("outputGain", "Output Gain",
        gainRange, 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("dB")
            .withStringFromValueFunction ([](float v, int) {
                return v <= -99.5f ? juce::String ("-inf")
                                   : juce::String (v, 1);
            })));

    params.push_back(std::make_unique<juce::AudioParameterFloat>("mix", "Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.5f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterBool>("compMode", "Mode (Smooth)", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("character", "Character (Nuke)", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("zeroLat", "Zero Latency", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>("fat", "FAT (x8)", false));
    return { params.begin(), params.end() };
}

// Helper: dB -> linear gain, with hard mute at very negative values.
// User-facing slider range goes down to -100 dB; anything below -99.5 dB is
// treated as absolute silence so the user can completely shut off the channel.
static inline float dbToGainWithMute (float dB) noexcept
{
    return dB <= -99.5f ? 0.0f : juce::Decibels::decibelsToGain (dB);
}

// ════════════════════════════════════════════════════════════════════════════
//  Konstruktor / destruktor
// ════════════════════════════════════════════════════════════════════════════
ABI1176Processor::ABI1176Processor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    bypassParam     = apvts.getRawParameterValue ("bypass");
    inputGainParam  = apvts.getRawParameterValue ("inputGain");
    driveParam      = apvts.getRawParameterValue ("drive");
    outputGainParam = apvts.getRawParameterValue ("outputGain");
    mixParam        = apvts.getRawParameterValue ("mix");
    compModeParam   = apvts.getRawParameterValue ("compMode");
    characterParam  = apvts.getRawParameterValue ("character");
    zeroLatParam    = apvts.getRawParameterValue ("zeroLat");
    fatParam        = apvts.getRawParameterValue ("fat");

    // KRYTYCZNE: rejestracja listenera. Bez tego parameterChanged nigdy się nie wywołuje
    // i oversampler nie reaguje na zmiany ZeroLat / FAT w trakcie playbacku.
    apvts.addParameterListener ("zeroLat", this);
    apvts.addParameterListener ("fat",     this);
}

ABI1176Processor::~ABI1176Processor()
{
    apvts.removeParameterListener ("zeroLat", this);
    apvts.removeParameterListener ("fat",     this);
}

// ════════════════════════════════════════════════════════════════════════════
//  Inicjalizacja oversamplera
// ════════════════════════════════════════════════════════════════════════════
void ABI1176Processor::updateOversampling()
{
    const bool useZeroLat = zeroLatParam->load() > 0.5f;
    const bool useFat     = fatParam->load()     > 0.5f;

    // FAT = x8 (faktor 3, bo 2^3=8), normalny = x4 (faktor 2)
    const int factor = useFat ? 3 : 2;

    // ZeroLat -> IIR halfband (krótka latencja faktyczna, ale faza nie jest liniowa)
    // Normalny -> FIR equiripple (lepsze tłumienie aliasingu kosztem latencji)
    const auto type = useZeroLat
        ? juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR
        : juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple;

    // Czwarty argument 'isMaxQuality' = !useZeroLat (dłuższe filtry FIR gdy nie ZeroLat)
    oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        2, factor, type, !useZeroLat);
    oversampler->initProcessing (static_cast<size_t> (currentSamplesPerBlock));

    const int latency = juce::roundToInt (oversampler->getLatencyInSamples());
    setLatencySamples (latency);

    // KLUCZOWA POPRAWKA: opóźnij dry o tę samą liczbę próbek
    resizeDryDelay (latency, 2);
}

// ════════════════════════════════════════════════════════════════════════════
//  Dry delay - circular buffer (potęga 2 dla szybkiego maskowania)
// ════════════════════════════════════════════════════════════════════════════
void ABI1176Processor::resizeDryDelay (int latencySamples, int numChannels)
{
    dryDelayLength = juce::jmax (0, latencySamples);

    // Bufor musi zmieścić: historię (dryDelayLength) + bieżący blok (currentSamplesPerBlock)
    // + zapas. Plus zaokrąglamy w górę do potęgi 2 dla szybkiego maskowania.
    const int needed = dryDelayLength + juce::jmax (currentSamplesPerBlock, 64) + 16;
    int size = 1;
    while (size < needed) size <<= 1;
    if (size < 64) size = 64;

    dryDelaySize = size;
    dryDelayMask = size - 1;
    dryDelayBuf.assign (static_cast<size_t> (numChannels),
                        std::vector<float> (static_cast<size_t> (size), 0.0f));
    dryDelayWrite = 0;
}

void ABI1176Processor::resetDryDelay()
{
    for (auto& ch : dryDelayBuf)
        std::fill (ch.begin(), ch.end(), 0.0f);
    dryDelayWrite = 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  prepareToPlay
// ════════════════════════════════════════════════════════════════════════════
void ABI1176Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate      = sampleRate;
    currentSamplesPerBlock = samplesPerBlock;

    // Smoothing
    inputGainSmooth .reset (sampleRate, 0.02);
    outputGainSmooth.reset (sampleRate, 0.02);
    driveSmooth     .reset (sampleRate, 0.01);
    mixSmooth       .reset (sampleRate, 0.02);

    inputGainSmooth .setCurrentAndTargetValue (dbToGainWithMute (inputGainParam->load()));
    outputGainSmooth.setCurrentAndTargetValue (dbToGainWithMute (outputGainParam->load()));
    driveSmooth     .setCurrentAndTargetValue (driveParam->load());
    mixSmooth       .setCurrentAndTargetValue (mixParam->load() / 100.0f);

    // Stałe czasowe wg schematu UREI 1176 (Rev A "Blue Stripe"):
    //   Attack  : 20 us (najszybszy) ... 800 us (najwolniejszy)
    //   Release : 50 ms (najszybszy) ... 1100 ms (najwolniejszy)
    // Dla drum-busa wybieramy środkowe, muzykalne wartości:
    //   ~150 us attack / ~200 ms release - zostawia transjent, kontroluje sustain.
    attackCoeff  = std::exp (-1.0f / (float (sampleRate) * 0.00015f));  // 150 us
    releaseCoeff = std::exp (-1.0f / (float (sampleRate) * 0.20f));     // 200 ms

    updateOversampling();
    oversamplerDirty.store (false);

    const double osRate = sampleRate * static_cast<double> (oversampler->getOversamplingFactor());

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = osRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock * oversampler->getOversamplingFactor());
    spec.numChannels      = 2;

    // Tor wyjściowy - emulacja transformatora Carnhill (Neve-style):
    //  - 1) HPF 15 Hz - DC-block po klipperze, lekko rozszerzony zakres basów
    //  - 2) Low-shelf 100 Hz, +3 dB, Q=0.6 - charakterystyczna "warmth" Carnhilla
    //  - 3) High-shelf 10 kHz, -1.5 dB, Q=0.7 - łagodny roll-off wysokich, miękki ton
    filterChain.get<0>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass  (osRate, 15.0f);
    filterChain.get<1>().state = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (osRate, 100.0f, 0.6f, juce::Decibels::decibelsToGain (3.0f));
    filterChain.get<2>().state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (osRate, 10000.0f, 0.7f, juce::Decibels::decibelsToGain (-1.5f));
    filterChain.prepare (spec);

    envFollower       = 0.0f;
    currentGR.store (0.0f);
    lastGainReduction = 1.0f;
    wasBypassed       = true;

    resetDryDelay();
}

void ABI1176Processor::releaseResources() {}

// ════════════════════════════════════════════════════════════════════════════
//  Modele saturacji - zachowane wszystkie oryginalne sygnatury i logika
// ════════════════════════════════════════════════════════════════════════════
float ABI1176Processor::processFETSaturation (float sample, float drive)
{
    // FET soft-clip - charakterystyka tranzystora 2N3819 z 1176
    float threshold = 0.3f + (1.0f - drive) * 0.5f;
    if (std::abs (sample) < threshold) return sample;
    float sign = (sample >= 0.0f) ? 1.0f : -1.0f;
    float x    = (std::abs (sample) - threshold) / (1.0f - threshold);
    return sign * (threshold + (1.0f - threshold)
                   * (std::tanh (x * (2.0f + drive * 3.0f))
                      / std::tanh (2.0f + drive * 3.0f)));
}

float ABI1176Processor::processTransformerSaturation (float sample, float amount)
{
    // Transformator wyjściowy Carnhill (Neve-style) - emulacja saturacji rdzenia.
    //
    // Klucz do "Carnhill sound" to OBECNOŚĆ drugiej harmonicznej (2nd order).
    // Czyste tanh(x) jest symetryczne i produkuje wyłącznie nieparzyste harmoniki
    // (3, 5, 7 ...) - to jest charakter Lundahla.  Aby wytworzyć drugą harmonikę
    // musimy lekko ZŁAMAĆ symetrię - dodajemy małe przesunięcie DC przed nieliniowością
    // i kompensujemy je za nią, tak żeby nie powstał statyczny offset DC w wyjściu.
    //
    // 'amount' steruje zarówno intensywnością saturacji jak i ilością asymetrii.
    const float drive       = 1.0f + amount * 1.5f;
    const float asymmetry   = amount * 0.15f;            // im więcej saturacji, tym więcej 2nd order
    const float normalizer  = std::tanh (drive);          // utrzymuje stałe wzmocnienie szczytowe
    const float dcCorrection = std::tanh (asymmetry * drive) / normalizer;

    return std::tanh ((sample + asymmetry) * drive) / normalizer - dcCorrection;
}

float ABI1176Processor::processAsymmetricClip (float sample, float bias)
{
    // Klasa A FET - asymetria; DC compensation aby nie wprowadzać offsetu
    const float exactDcOffset = std::tanh (bias * 0.08f * 1.5f) / std::tanh (1.5f);
    const float biased        = sample + bias * 0.08f;
    const float clipped       = std::tanh (biased * 1.5f) / std::tanh (1.5f);
    return clipped - exactDcOffset;
}

// ════════════════════════════════════════════════════════════════════════════
//  processBlock - GŁÓWNA NAPRAWA PHASERA
// ════════════════════════════════════════════════════════════════════════════
void ABI1176Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples             = buffer.getNumSamples();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, numSamples);

    // Bezpieczna przebudowa oversamplera (gdy parameterChanged zaznaczył 'dirty')
    if (oversamplerDirty.exchange (false))
    {
        updateOversampling();
        // Filtry chain trzeba przygotować na nowy rate
        const double osRate = currentSampleRate * static_cast<double> (oversampler->getOversamplingFactor());
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = osRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (currentSamplesPerBlock * oversampler->getOversamplingFactor());
        spec.numChannels      = 2;
        filterChain.get<0>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass  (osRate, 15.0f);
        filterChain.get<1>().state = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (osRate, 100.0f, 0.6f, juce::Decibels::decibelsToGain (3.0f));
        filterChain.get<2>().state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (osRate, 10000.0f, 0.7f, juce::Decibels::decibelsToGain (-1.5f));
        filterChain.prepare (spec);
        filterChain.reset();
        resetDryDelay();
        envFollower       = 0.0f;
        lastGainReduction = 1.0f;
    }

    const bool isBypassed = bypassParam->load() > 0.5f;
    if (isBypassed)
    {
        currentGR.store (0.0f);
        wasBypassed = true;
        // Mimo bypassu - karmimy dry-delay, żeby po wyjściu z bypassu nie było clicka
        // (ale tylko jeśli mamy linię opóźniającą)
        if (dryDelayLength > 0 && totalNumInputChannels > 0)
        {
            for (int s = 0; s < numSamples; ++s)
            {
                for (int ch = 0; ch < juce::jmin (totalNumInputChannels, (int) dryDelayBuf.size()); ++ch)
                    dryDelayBuf[(size_t) ch][(size_t) dryDelayWrite] = buffer.getReadPointer (ch)[s];
                dryDelayWrite = (dryDelayWrite + 1) & dryDelayMask;
            }
        }
        return;
    }

    // ─── 1. Skopiuj dry PRZED jakimkolwiek przetwarzaniem ────────────────────
    juce::AudioBuffer<float> dryBuffer;
    dryBuffer.makeCopyOf (buffer);

    if (wasBypassed)
    {
        inputGainSmooth .setCurrentAndTargetValue (dbToGainWithMute (inputGainParam->load()));
        outputGainSmooth.setCurrentAndTargetValue (dbToGainWithMute (outputGainParam->load()));
        driveSmooth     .setCurrentAndTargetValue (driveParam->load());
        mixSmooth       .setCurrentAndTargetValue (mixParam->load() / 100.0f);
        filterChain.reset();
        oversampler->reset();
        envFollower       = 0.0f;
        lastGainReduction = 1.0f;
        resetDryDelay();
        wasBypassed = false;
    }
    else
    {
        inputGainSmooth .setTargetValue (dbToGainWithMute (inputGainParam->load()));
        outputGainSmooth.setTargetValue (dbToGainWithMute (outputGainParam->load()));
        driveSmooth     .setTargetValue (driveParam->load());
        mixSmooth       .setTargetValue (mixParam->load() / 100.0f);
    }

    // ─── 2. Zapis dry do linii opóźniającej (przed up-sample) ────────────────
    if (dryDelayLength > 0 && totalNumInputChannels > 0)
    {
        const int chCount = juce::jmin (totalNumInputChannels, (int) dryDelayBuf.size());
        for (int s = 0; s < numSamples; ++s)
        {
            for (int ch = 0; ch < chCount; ++ch)
                dryDelayBuf[(size_t) ch][(size_t) dryDelayWrite]
                    = dryBuffer.getReadPointer (ch)[s];
            dryDelayWrite = (dryDelayWrite + 1) & dryDelayMask;
        }
    }

    // ─── 3. Oversampling UP ──────────────────────────────────────────────────
    juce::dsp::AudioBlock<float> inputBlock (buffer);
    juce::dsp::AudioBlock<float> oversampledBlock = oversampler->processSamplesUp (inputBlock);

    const int  osSamples    = (int) oversampledBlock.getNumSamples();
    const bool isSmoothMode = compModeParam->load() > 0.5f;
    const bool isNukeMode   = characterParam->load() > 0.5f;
    float blockMinGR        = 1.0f;

    // ─── 4. Kompresja w przestrzeni oversamplowanej ──────────────────────────
    //
    // Topologia 1176 (zgodna ze schematem UREI Rev F + masonaudio.org/comp1176):
    //
    //  ORYGINALNY 1176 (cały modelowy łańcuch produkcji 1967-1973) jest TYLKO
    //  FEEDBACK. Cytat z masonaudio: "The signal used for gain control is sensed
    //  after the gain reduction (feedback style compression) and it is fed to
    //  the gate of the FET."  Feedback = wolniejszy, gładszy attack, brzmi
    //  "klasycznie" i "muzykalnie".
    //
    //  Wiele NOWOCZESNYCH klonów 1176 (Waves CLA-76, Klanghelm MJUC itd.)
    //  oferuje przełącznik FF/FB jako dodatek - feedforward daje szybszy,
    //  bardziej "punchy" attack, lepszy dla drum-busa.
    //
    //  Mapowanie naszego przełącznika "SMOOTH":
    //    SMOOTH ON  -> klasyczny FEEDBACK (oryginalny 1176, "gładki, classic")
    //    SMOOTH OFF -> FEEDFORWARD (modyfikacja modern, "szybszy, punchy")
    //
    //  Przełącznik "NUKE":
    //    NUKE OFF -> ratio sterowany przez 'drive' (≈ 4:1 do ≈ 20:1)
    //    NUKE ON  -> BRICKWALL (∞:1) - emulacja "all-buttons-in" / British Mode
    //
    for (int sample = 0; sample < osSamples; ++sample)
    {
        const float inputGain = inputGainSmooth.getNextValue();
        const float drive     = driveSmooth    .getNextValue();

        // Średnia mono detektora (suma L+R) - klasyczny stereo-linked 1176
        float det = 0.0f;
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
            det += std::abs (oversampledBlock.getSample (channel, sample)) * inputGain;
        if (totalNumInputChannels > 1) det *= 0.5f;

        // SMOOTH = feedback (sygnał detekcji * aktualne GR -> "po GR")
        // Bez SMOOTH = feedforward (czysty sygnał wejściowy do detektora)
        const float detIn = isSmoothMode ? det * lastGainReduction : det;

        // Envelope follower - dwustanowy attack/release
        if (detIn > envFollower)
            envFollower = attackCoeff  * envFollower + (1.0f - attackCoeff)  * detIn;
        else
            envFollower = releaseCoeff * envFollower + (1.0f - releaseCoeff) * detIn;

        // Krzywa GR - próg 0.25 (-12 dBFS), ratio sterowany przez 'drive'.
        // NUKE = brickwall (∞:1): wyjście jest TWARDO klamrowane do progu.
        //   Dla envFollower > threshold:  gr = threshold / envFollower
        //   => output = input * gr = (input/inputGain) * (threshold/envFollower)
        //              ≈ threshold (gdy envFollower ≈ inputGain*|sample|)
        float gr = 1.0f;
        if (envFollower > 0.25f)
        {
            gr = isNukeMode
                ? (0.25f / envFollower)
                : (1.0f / (1.0f + (envFollower - 0.25f) * 15.0f * drive));
        }
        lastGainReduction = gr;
        if (gr < blockMinGR) blockMinGR = gr;

        // Aplikacja GR + saturacje na każdym kanale
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
        {
            float x = oversampledBlock.getSample (channel, sample) * inputGain * gr;
            x = processFETSaturation       (x, drive);
            x = processAsymmetricClip      (x, 0.5f);
            x = processTransformerSaturation (x, 0.3f);
            oversampledBlock.setSample (channel, sample, x);
        }
    }

    // ── VU meter calibration ────────────────────────────────────────────────
    // blockMinGR to NAJMNIEJSZY mnożnik 'gr' w bloku (im mniejszy, tym większa
    // redukcja). gainToDecibels(0.5) = -6 dB, więc 6 dB redukcji daje
    // currentGR = -6 dB.  W edytorze:
    //     grNorm = -currentGR / 20.0  -> mapuje 0..-20 dB GR na 0..1
    //     needleAngle = +0.78 + grNorm * (-0.78 - +0.78)
    // Skala edytora ma kreski przy 0/5/10/15/20 dB i mapowanie jest LINIOWE,
    // więc rzeczywista redukcja o 5 dB wskazuje DOKŁADNIE na liczbę "5" na skali.
    // To jest świadome uproszczenie - oryginalny mechaniczny VU 1176 stosuje
    // lekko logarytmiczne mapowanie (0-3 dB ściśnięte, 15-20 dB rozciągnięte),
    // ale wersja liniowa daje czytelniejszy odczyt dla operatora.
    float targetGR_dB = juce::Decibels::gainToDecibels (blockMinGR);
    if (targetGR_dB < -40.0f) targetGR_dB = -40.0f;
    if (targetGR_dB > 0.0f)   targetGR_dB = 0.0f;
    currentGR.store (targetGR_dB);

    // ─── 5. Tor toneshapingu (HPF + low-shelf) na rate oversamplowanym ──────
    juce::dsp::ProcessContextReplacing<float> context (oversampledBlock);
    filterChain.process (context);

    // ─── 6. Downsampling - wynik trafia z powrotem do 'buffer' ──────────────
    oversampler->processSamplesDown (inputBlock);

    // ─── 7. Mix + output gain - z DRY-DELAY ──────────────────────────────────
    //
    // Tutaj wcześniej pojawiał się phaser: dryBuffer był nieopóźniony,
    // a buffer (wet) wracał z oversamplera spóźniony o 'dryDelayLength' próbek.
    // Mieszanie => filtr grzebieniowy => "phaser" na drum-busie.
    //
    // Naprawa: pobieramy dry z circular buffera, opóźnionego dokładnie tyle
    // ile wynosi latencja oversamplera.
    //
    if (dryDelayLength > 0)
    {
        const int chCount = juce::jmin (totalNumInputChannels, (int) dryDelayBuf.size());
        // Odczyt zaczyna się 'dryDelayLength' próbek za pozycją zapisu sprzed bloku.
        // Po pętli zapisu (krok 2) write wskazuje "next" -> chcemy odczyt o numSamples
        // wstecz od bieżącego write, minus jeszcze dryDelayLength.
        // Najprościej: utrzymujemy oddzielny "readPos" = write - numSamples - dryDelayLength
        int readPos = (dryDelayWrite - numSamples - dryDelayLength) & dryDelayMask;

        for (int s = 0; s < numSamples; ++s)
        {
            const float m = mixSmooth      .getNextValue();
            const float o = outputGainSmooth.getNextValue();
            for (int ch = 0; ch < chCount; ++ch)
            {
                const float dry = dryDelayBuf[(size_t) ch][(size_t) readPos];
                const float wet = buffer.getReadPointer (ch)[s];
                buffer.getWritePointer (ch)[s] = (dry * (1.0f - m) + wet * m) * o;
            }
            readPos = (readPos + 1) & dryDelayMask;
        }
    }
    else
    {
        // Brak latencji (ZeroLat IIR może mieć 0) - prosty mix
        for (int s = 0; s < numSamples; ++s)
        {
            const float m = mixSmooth      .getNextValue();
            const float o = outputGainSmooth.getNextValue();
            for (int ch = 0; ch < totalNumInputChannels; ++ch)
                buffer.getWritePointer (ch)[s]
                    = (dryBuffer.getReadPointer (ch)[s] * (1.0f - m)
                     + buffer   .getReadPointer (ch)[s] * m) * o;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  parameterChanged - tylko ZAZNACZA flagę, nie przebudowuje od razu.
//  Faktyczna przebudowa odbywa się w processBlock (na audio threadzie).
//  To zapobiega race condition i trzaskom.
// ════════════════════════════════════════════════════════════════════════════
void ABI1176Processor::parameterChanged (const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused (newValue);
    if (parameterID == "zeroLat" || parameterID == "fat")
        oversamplerDirty.store (true);
}

// ════════════════════════════════════════════════════════════════════════════
//  State (zachowane bez zmian)
// ════════════════════════════════════════════════════════════════════════════
void ABI1176Processor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void ABI1176Processor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr && xmlState->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

bool ABI1176Processor::hasEditor() const { return true; }

juce::AudioProcessorEditor* ABI1176Processor::createEditor()
{
    return new ABI1176Editor (*this, apvts);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ABI1176Processor();
}