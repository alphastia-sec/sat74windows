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
        juce::NormalisableRange<float>(0.0f, 1.5f, 0.01f), 0.5f));

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
    params.push_back(std::make_unique<juce::AudioParameterBool>("fat", "FAT (x16)", false));
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

    // FAT = x16 (faktor 4, bo 2^4=16), normalny = x4 (faktor 2)
    // x16 daje ekstremalnie dobre tłumienie aliasingu (potrzebne przy mocnym
    // saturatorze i wielu harmonicznych), kosztem ~4× więcej CPU vs x4.
    const int factor = useFat ? 4 : 2;

    // ZeroLat -> IIR halfband (krótka latencja faktyczna, ale faza nie jest liniowa)
    // Normalny -> FIR equiripple (lepsze tłumienie aliasingu kosztem latencji)
    const auto type = useZeroLat
        ? juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR
        : juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple;

    // Czwarty argument 'isMaxQuality' = !useZeroLat (dłuższe filtry FIR gdy nie ZeroLat)
    oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        2, factor, type, !useZeroLat);
    oversampler->initProcessing (static_cast<size_t> (currentSamplesPerBlock));

    const int osLatency = juce::roundToInt (oversampler->getLatencyInSamples());

    // B1: lookahead.  Długość w przestrzeni osRate = 1.5 ms × osRate.
    // osRate = hostSR × factor.  factor = 2^? gdzie ? to 'factor' (2 lub 4).
    const int osMultiplier = 1 << factor;     // 4 (normal) lub 16 (FAT)
    const double osRate    = currentSampleRate * static_cast<double> (osMultiplier);
    lookaheadLength = juce::roundToInt (0.0015 * osRate);   // 1.5 ms

    // Bufor lookahead - circular, potęga 2
    {
        const int needed = lookaheadLength
                          + juce::jmax (currentSamplesPerBlock * osMultiplier, 64) + 16;
        int size = 1;
        while (size < needed) size <<= 1;
        if (size < 64) size = 64;
        lookaheadSize  = size;
        lookaheadMask  = size - 1;
        lookaheadBuf.assign (2, std::vector<float> (static_cast<size_t> (size), 0.0f));
        lookaheadWrite = 0;
    }

    // Całkowita latencja zgłaszana hostowi:
    //   - oversampler latency (w przestrzeni host)
    //   - lookahead (w przestrzeni osRate → konwersja na host: / osMultiplier)
    const int lookaheadHostSamples = lookaheadLength / osMultiplier;
    const int totalLatency = osLatency + lookaheadHostSamples;
    setLatencySamples (totalLatency);

    // KLUCZOWA POPRAWKA: opóźnij dry o tę samą liczbę próbek (cała latencja)
    resizeDryDelay (totalLatency, 2);
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

    // Oversampler MUSI być zainicjalizowany jako pierwszy - smoothery, detector
    // i filtry potrzebują znać osRate.
    updateOversampling();
    oversamplerDirty.store (false);

    const double osRate = sampleRate * static_cast<double> (oversampler->getOversamplingFactor());

    // ── Parameter smoothers ─────────────────────────────────────────────────
    // Smoothery są używane WEWNĄTRZ pętli oversamplowanej, więc 'sampleRate'
    // przekazywane do reset() musi być rate'em oversamplera, nie hosta.
    // W przeciwnym razie rampa kończy się N× szybciej niż zaplanowano
    // (N = czynnik oversamplingu), co przy szybkim ruchu DRIVE produkuje
    // zipper-noise / clicki.
    //
    // Stała czasowa 50 ms - dostatecznie długa by ukryć zipper, dość krótka
    // by ruch knobu był responsywny.
    inputGainSmooth .reset (osRate, 0.05);
    outputGainSmooth.reset (osRate, 0.05);
    driveSmooth     .reset (osRate, 0.05);
    mixSmooth       .reset (osRate, 0.05);

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

    // osRate już policzony na początku funkcji - używamy lokalnej kopii.

    // Detector LPF — usuwa tętnienie audio-rate z prostowanego sygnału.
    // 2-pole RC kaskada (12 dB/okt) - mocniej tłumi ripple 2·f0 niż 1-pole.
    {
        const float fc = 25.0f;
        const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
        const float dt = 1.0f / static_cast<float> (osRate);
        detectorLPFCoeff = dt / (rc + dt);
        detectorLPF[0]   = 0.0f;
        detectorLPF[1]   = 0.0f;
    }

    // GR smoothing LPF (3-pole, 100 Hz) - wygładza mnożnik gain reduction
    // przed aplikacją do audio, eliminując sidebands modulacyjne (fałszywe
    // H3/H5 + LF "falowanie").  3-pole = 18 dB/okt, bardzo mocne tłumienie
    // ripple 2·f0 przy zachowaniu szybkiego attacku kompresji (< 100 Hz).
    {
        const float fc = 100.0f;
        const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
        const float dt = 1.0f / static_cast<float> (osRate);
        grSmoothCoeff   = dt / (rc + dt);
        grSmoothLPF[0]  = 1.0f;
        grSmoothLPF[1]  = 1.0f;
        grSmoothLPF[2]  = 1.0f;
    }

    // A1: Side-chain HPF (juce::dsp::IIR, 2-pole, 80 Hz, Q=0.707).
    // Tłumi bas w torze DETEKTORA (audio path nie tknięty), eliminując
    // pompowanie basowe i sidebands modulacyjne w paśmie LF.
    {
        sidechainHPF.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, 80.0f, 0.707f);
        juce::dsp::ProcessSpec scSpec;
        scSpec.sampleRate       = osRate;
        scSpec.maximumBlockSize = 1;          // przetwarzamy po jednej próbce
        scSpec.numChannels      = 1;          // mono detektor
        sidechainHPF.prepare (scSpec);
        sidechainHPF.reset();
    }

    // A2: usunęliśmy transformerDC LPF - nowy fizyczny model transformera
    // (asymetryczny Padé approximant) nie produkuje DC offsetu, więc nie
    // potrzebujemy adaptacyjnego estymatora <x²>.
    //
    // Osobny DC blocker również usunięty - blokowanie DC zapewnia HPF
    // 6-rzędu 45 Hz w filterChain (poniżej).  Patrz komentarz w pętli
    // kompresji przy "BLOKOWANIE DC".

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = osRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock * oversampler->getOversamplingFactor());
    spec.numChannels      = 2;

    // Tor wyjściowy - HPF 6-rzędu (LF cleanup) + emulacja Carnhill:
    //
    //  Sloty 0,1,2: HPF 6-rzędu Butterworth, cutoff 45 Hz.
    //    6-rzędu = 3 kaskadowe biquady 2-rzędu o tym samym cutoff, różnych Q.
    //    Wartości Q dla Butterworth 6-rzędu: 0.5176, 0.7071, 1.9319.
    //    Tłumienie: -42 dB @ 20 Hz, -31 dB @ 25 Hz, 0.00 dB @ 260 Hz.
    //    Eliminuje WSZYSTKO poniżej fundamentalnej drum-busa - sub-bass
    //    artefakty, modulacyjny LF, leakage analizatora - poniżej -120 dB.
    //    Cutoff 45 Hz jest bezpieczny dla drum-busa (kick ma fundament
    //    50-60 Hz - nietknięty; odcinamy tylko niepożądany sub < 40 Hz).
    //
    //  Slot 3: Low-shelf 120 Hz, +1.5 dB - delikatna "warmth" Carnhilla
    //  Slot 4: High-shelf 12 kHz, -1.2 dB - łagodny "air" roll-off Carnhilla
    {
        const float hpfFc = 45.0f;
        filterChain.get<0>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, hpfFc, 0.5176f);
        filterChain.get<1>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, hpfFc, 0.7071f);
        filterChain.get<2>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, hpfFc, 1.9319f);
    }
    filterChain.get<3>().state = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (osRate, 120.0f, 0.7f, juce::Decibels::decibelsToGain (1.5f));
    filterChain.get<4>().state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (osRate, 12000.0f, 0.7f, juce::Decibels::decibelsToGain (-1.2f));
    filterChain.prepare (spec);

    // Crossfade length: 30 ms w sample rate hosta - długo dosyć by ukryć
    // wszystkie transienty filtrów i krótkich attacków, ale nie tak długo
    // by user zauważał "zanikanie" przy włączaniu bypassu.
    crossfadeLength  = static_cast<int> (sampleRate * 0.030);
    crossfadeCounter = 0;

    envFollower       = 0.0f;
    currentGR.store (0.0f);
    lastGainReduction = 1.0f;
    wasBypassed       = true;

    resetDryDelay();
}

void ABI1176Processor::releaseResources() {}

// ════════════════════════════════════════════════════════════════════════════
//  Modele saturacji
//
//  Cel: generować bogate spektrum harmonicznych (jak prawdziwy 1176 i UAD),
//  unikając jednocześnie infrasonic noise od asymetrycznych nieliniowości.
//
//  Stary kod używał trzech kaskadowych tanh-ów (FET + Asym + Transformer).
//  Każde tanh ostro tłumi harmoniki powyżej 3rd/5th, więc po trzech etapach
//  dostawaliśmy tylko 5-6 słabych harmonik — zamiast 15+ jak UAD.
//
//  Nowy kod: jedno mocne ogniwo nieliniowe (waveshaper FET-style produkujący
//  szerokie spektrum) + bardzo subtelny transformator który DODAJE 2nd order,
//  ale nie wygładza istniejących harmonik.
// ════════════════════════════════════════════════════════════════════════════
float ABI1176Processor::processFETSaturation (float sample, float drive)
{
    // FET cut-off charakteristic (2N3819 w 1176 GR-stage).
    //
    // W nowej topologii (parallel saturation) ta funkcja jest wywoływana z
    // STAŁYM drive=1.0 jako "referencja pełnej saturacji".  Sam parametr drive
    // jest zachowany w sygnaturze dla kompatybilności, ale wewnętrzny 'gain'
    // jest stały (0.8) by zapewnić wyraźne harmoniki dla parallel-mix.
    //
    // Topologia: ASYMETRYCZNY SATURATOR  x → x / (1 + |x|·k).  Fizyczny model
    // FET-a w stanie cut-off (drain-source nasyca się asymptotycznie).
    // Generuje pełną serię NIEPARZYSTYCH harmonik (3rd, 5th, 7th, 9th, ...).
    //
    // KRYTYCZNE: funkcja jest IDEALNIE SYMETRYCZNA (nieparzysta).  Każde
    // wprowadzenie asymetrii (bias DC, asymetryczne knee) NATYCHMIAST
    // produkuje parzyste harmoniki dominujące nad nieparzystymi.  2nd order
    // content (Carnhill) dodajemy WYŁĄCZNIE w processTransformerSaturation.
    juce::ignoreUnused (drive);
    const float gain      = 1.20f;        // mocniejszy gain (było 0.80)
                                          // wzmacnia H3, H5, H7... bliżej Antelope FET
    const float satCoeff  = 0.5f;

    const float x = sample * gain;

    float y;
    if (x >= 0.0f)  y = x / (1.0f + x * satCoeff);
    else            y = x / (1.0f - x * satCoeff);

    return y / gain;     // unity gain dla małych sygnałów
}

float ABI1176Processor::processTransformerSaturation (float sample, float amount, int channelIdx)
{
    // A2: Carnhill output transformer model - FIZYCZNY MODEL ASYMETRYCZNEJ
    // KRZYWEJ NASYCENIA RDZENIA (B-H hysteresis).
    //
    // Krzywa: y = x / (1 + β·x)        dla x ≥ 0  (gałąź dodatnia)
    //         y = x / (1 - β'·x)       dla x < 0  (gałąź ujemna, β' = β·0.7)
    //
    // Dlaczego ten model jest fizycznie poprawny:
    //   - W transformatorze rdzenie żelazowe (jak Carnhill VTB1) mają
    //     ASYMETRYCZNĄ krzywą magnetyzacji B-H: jedna połówka cyklu ma trochę
    //     inną krzywiznę niż druga.  To wynika z hysteresis loop + dipolarnej
    //     orientacji domen Weissa.
    //   - Padé approximant `x / (1 + β·x)` jest fizycznym przybliżeniem
    //     soft-clip diody/półprzewodnika z różną krzywizną dla dodatnich/ujemnych.
    //   - Asymetria w β (β'=β·0.7) generuje 2nd harmonic NATURALNIE,
    //     bez DC offsetu i bez aktywnej kompensacji LPF (która produkowała
    //     biały szum LF w poprzednich wersjach).
    //
    // Wybór β = 0.20·amount:
    //   - amount=0.5 → β=0.10 → H2=-49 dB, H3=-43 dB (subtelne)
    //   - amount=1.0 → β=0.20 → H2=-43 dB, H3=-37 dB (umiarkowane, ~Antelope)
    //   - amount=1.5 → β=0.30 → H2=-40 dB, H3=-34 dB (mocne, kreatywne)
    //
    // KRYTYCZNE: zero DC offsetu w stanie ustalonym - to eliminuje LF noise
    // floor produkowany wcześniejszą wersją z aktywną kompensacją <x²>.

    juce::ignoreUnused (channelIdx);
    const float beta_pos = 0.20f * amount;        // gałąź dodatnia
    const float beta_neg = beta_pos * 0.7f;       // gałąź ujemna (asymetria → 2nd harmonic)

    float x_with_2nd;
    if (sample >= 0.0f)
        x_with_2nd = sample / (1.0f + beta_pos * sample);
    else
        x_with_2nd = sample / (1.0f - beta_neg * sample);

    // 2) Łagodna saturacja core - tylko gdy sygnał osiąga thresh
    const float thresh = 0.7f;
    const float absA   = std::abs (x_with_2nd);

    if (absA < thresh)
        return x_with_2nd;

    // Soft kompresja powyżej threshold (symetryczna - dodaje nieparzyste)
    const float excess = absA - thresh;
    const float bend   = excess / (1.0f + excess * amount * 2.0f);
    const float sign   = (x_with_2nd >= 0.0f) ? 1.0f : -1.0f;
    return sign * (thresh + bend);
}

float ABI1176Processor::processAsymmetricClip (float sample, float bias)
{
    // Mimo nazwy historycznej, ta funkcja jest teraz SYMETRYCZNYM "polish"
    // saturatorem na wysokim poziomie sygnału.  Każda asymetria wprowadzona
    // tutaj manifestowała się jako alternacja H2/H4 dominująca nad H3/H5 -
    // klasyczny błąd 1176-style pluginów.
    //
    // Funkcja: symetryczne, asymptotic kompresyjne kolanko od ±0.5 amplitudy,
    // które dodaje delikatne zaokrąglenie szczytów (kolejne nieparzyste
    // harmoniki) bez wprowadzania asymetrii.
    juce::ignoreUnused (bias);

    const float absS = std::abs (sample);
    if (absS < 0.5f) return sample;                     // dolne 50 % bez modyfikacji

    const float excess = absS - 0.5f;
    const float sign   = (sample >= 0.0f) ? 1.0f : -1.0f;
    const float comp   = excess / (1.0f + excess * 0.8f);

    // SYMETRYCZNE - identyczna krzywa dla + i -.  Brak asymetrii = brak parzystych.
    return sign * (0.5f + comp);
}

// ════════════════════════════════════════════════════════════════════════════
//  D2: processSaturationChain - cały łańcuch saturacji w jednej funkcji
//
//  Łączy FET → asymmetric clip → transformer Carnhill.  Wydzielone do osobnej
//  funkcji, bo D2 (mini-oversampling) wywołuje cały łańcuch 2× per próbkę.
// ════════════════════════════════════════════════════════════════════════════
float ABI1176Processor::processSaturationChain (float sample, int channelIdx)
{
    float s = sample;
    s = processFETSaturation         (s, 1.0f);
    s = processAsymmetricClip        (s, 0.5f);
    s = processTransformerSaturation (s, 0.5f, channelIdx);
    return s;
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
        {
            const float hpfFc = 45.0f;
            filterChain.get<0>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, hpfFc, 0.5176f);
            filterChain.get<1>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, hpfFc, 0.7071f);
            filterChain.get<2>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, hpfFc, 1.9319f);
        }
        filterChain.get<3>().state = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (osRate, 120.0f, 0.7f, juce::Decibels::decibelsToGain (1.5f));
        filterChain.get<4>().state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (osRate, 12000.0f, 0.7f, juce::Decibels::decibelsToGain (-1.2f));
        filterChain.prepare (spec);
        filterChain.reset();

        // Smoothery muszą znać nowy rate (osRate zmienił się o czynnik 2 przy
        // przełączeniu FAT, albo o latencję przy ZERO LAT) - inaczej rampy będą
        // miały złą długość i ruchy DRIVE/INPUT będą trzaskać.
        inputGainSmooth .reset (osRate, 0.05);
        outputGainSmooth.reset (osRate, 0.05);
        driveSmooth     .reset (osRate, 0.05);
        mixSmooth       .reset (osRate, 0.05);
        // Ustaw na aktualne wartości - żeby nie było rampy od poprzedniego stanu
        inputGainSmooth .setCurrentAndTargetValue (dbToGainWithMute (inputGainParam->load()));
        outputGainSmooth.setCurrentAndTargetValue (dbToGainWithMute (outputGainParam->load()));
        driveSmooth     .setCurrentAndTargetValue (driveParam->load());
        mixSmooth       .setCurrentAndTargetValue (mixParam->load() / 100.0f);

        // Detector LPF i DC blocker zależne od OS rate — przelicz na nowo
        {
            const float fc = 25.0f;
            const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
            const float dt = 1.0f / static_cast<float> (osRate);
            detectorLPFCoeff = dt / (rc + dt);
            detectorLPF[0]   = 0.0f;
            detectorLPF[1]   = 0.0f;
        }
        // GR smoothing LPF - przelicz dla nowego osRate
        {
            const float fc = 100.0f;
            const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
            const float dt = 1.0f / static_cast<float> (osRate);
            grSmoothCoeff  = dt / (rc + dt);
            grSmoothLPF[0] = 1.0f;
            grSmoothLPF[1] = 1.0f;
            grSmoothLPF[2] = 1.0f;
        }
        // Side-chain HPF - przelicz dla nowego osRate
        {
            sidechainHPF.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (osRate, 80.0f, 0.707f);
            juce::dsp::ProcessSpec scSpec;
            scSpec.sampleRate       = osRate;
            scSpec.maximumBlockSize = 1;
            scSpec.numChannels      = 1;
            sidechainHPF.prepare (scSpec);
            sidechainHPF.reset();
        }
        resetDryDelay();
        envFollower       = 0.0f;
        lastGainReduction = 1.0f;
        // Reset crossfade też - zmiana zeroLat/fat resetuje filtry i może
        // wytworzyć transient.  Crossfade go zamaskuje.
        crossfadeCounter  = crossfadeLength;
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
        // Reset detektora - aby nie mieć "duchów" z poprzednio skompresowanego
        // sygnału, gdy plugin wraca z bypassu
        detectorLPF[0] = detectorLPF[1] = 0.0f;
        sidechainHPF.reset();
        grSmoothLPF[0] = grSmoothLPF[1] = grSmoothLPF[2] = 1.0f;
        // D2: reset mini-oversampling state
        for (int c = 0; c < 2; ++c)
        {
            for (int k = 0; k < 4; ++k) { miniOsUpHist[c][k] = 0.0f; miniOsDownHist[c][k] = 0.0f; }
            cleanDelay[c][0] = cleanDelay[c][1] = 0.0f;
        }
        // B1: reset lookahead buffer
        for (auto& ch : lookaheadBuf)
            std::fill (ch.begin(), ch.end(), 0.0f);
        lookaheadWrite = 0;
        resetDryDelay();
        // Uruchom crossfade dry→active by ukryć transienty filtrów/oversamplera
        crossfadeCounter = crossfadeLength;
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
    // ─── 4. Kompresja w przestrzeni oversamplowanej ──────────────────────────
    //
    // ARCHITEKTURA v2 (rewriting po feedback od użytkownika):
    //
    // 1) Ratio jest STAŁE = 8:1 (NUKE off) lub ∞:1 brickwall (NUKE on).
    //    Drive NIE wpływa na ratio kompresji.
    //
    // 2) Drive kontroluje WYŁĄCZNIE głośność harmonik, nie fundamentalnej.
    //    Implementacja: parallel saturation path.
    //
    //         clean      = input * inputGain * gr                  (czysty wet)
    //         saturated  = saturate(clean)                         (z harmonikami)
    //         harmonics  = saturated - clean                       (TYLKO harmoniki)
    //         output_wet = clean + harmonics * drive
    //
    //    Drive = 0   → output = clean (zerowe harmoniki, czysta kompresja)
    //    Drive = 1   → output = saturated (pełna saturacja)
    //    Drive = 0.5 → harmoniki na połowie głośności
    //
    //    KLUCZOWE: różnica `saturated - clean` znosi wzajemnie wszystkie LF
    //    artefakty wspólne dla obu sygnałów (modulacja envelope, DC drift od
    //    transformera).  To eliminuje "garb LF" widoczny w obu trybach.
    //
    // 3) Topologia FF/FB (SMOOTH on = FB) bez zmian.
    //
    // 4) Hard clip realizujemy DOPIERO PO output_gain, w pętli mix (krok 7).
    //    Dzięki temu OUTPUT może podbić sygnał, ale nigdy nie przekroczy 0 dBFS.

    const float threshold = 0.25f;                    // -12 dBFS
    const float ratioConst = 8.0f;                    // STAŁE ratio 8:1 (jak w 1176 "8")
    // Krzywa GR dla ratio R:1 powyżej progu:
    //   output_level = threshold + (input_level - threshold) / R
    //   gr = output_level / input_level
    //      = (threshold + (env - threshold)/R) / env
    // Dla env=threshold gr=1, dla env→∞ gr→1/R (asymptota 8:1).

    for (int sample = 0; sample < osSamples; ++sample)
    {
        const float inputGain = inputGainSmooth.getNextValue();
        const float drive     = driveSmooth    .getNextValue();

        // ── B1: LOOKAHEAD - zapis bieżących próbek do circular buffera ───────
        // Detektor (poniżej) czyta sygnał BEZ opóźnienia = "patrzy w przyszłość".
        // Audio path (pętla saturacji) czyta z lookaheadBuf = sygnał opóźniony
        // o 'lookaheadLength' próbek.  Dzięki temu kompresor zaczyna redukcję
        // ZANIM transient dotrze do toru audio → zero overshoot.
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
            lookaheadBuf[(size_t) channel][(size_t) lookaheadWrite]
                = oversampledBlock.getSample (channel, sample);

        // Indeks odczytu opóźnionego (lookaheadLength próbek wstecz)
        const int laRead = (lookaheadWrite - lookaheadLength) & lookaheadMask;

        // Średnia mono detektora (suma L+R) - klasyczny stereo-linked 1176.
        // Detektor czyta BIEŻĄCĄ próbkę (nieopóźnioną) - to jest lookahead.
        float det = 0.0f;
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
            det += std::abs (oversampledBlock.getSample (channel, sample)) * inputGain;
        if (totalNumInputChannels > 1) det *= 0.5f;

        // ── Side-chain HPF (80 Hz, 2-pole) ──────────────────────────────────
        // Tłumi bas w torze DETEKTORA - kompresor "nie widzi" sygnałów < 80 Hz.
        // Efekty:
        //   1) Brak pompowania od kicka/basu (klasyczna technika 1176)
        //   2) Sidebands modulacyjne LF (od mnożenia signal*gr) są minimalne
        //      bo gr zmienia się TYLKO w odpowiedzi na pasma > 80 Hz
        //   3) Audio path NIE jest tknięty - SC HPF jest tylko w detektorze
        det = std::abs (sidechainHPF.processSample (det));

        // ── Pre-detector LPF (2-pole, ~25 Hz) ───────────────────────────────
        // Usuwa audio-rate ripple z prostowanego sygnału.  2-pole kaskada
        // (12 dB/okt) tłumi ripple 2·f0 mocniej niż 1-pole - mniej modulacji.
        detectorLPF[0] += detectorLPFCoeff * (det           - detectorLPF[0]);
        detectorLPF[1] += detectorLPFCoeff * (detectorLPF[0] - detectorLPF[1]);
        const float detSmoothed = detectorLPF[1];

        // SMOOTH = feedback (sygnał detekcji * aktualne GR -> "po GR")
        // Bez SMOOTH = feedforward (czysty sygnał wejściowy do detektora)
        //
        // UWAGA: NIE używamy LPF na lastGainReduction (testowane wcześniej -
        // wprowadzał opóźnienie w pętli sprzężenia, destabilizował pętlę i
        // powodował "wycie" przy wysokim drive).  W prawdziwym 1176 sprzężenie
        // jest natychmiastowe.
        const float detIn = isSmoothMode ? detSmoothed * lastGainReduction
                                         : detSmoothed;

        // Envelope follower - dwustanowy attack/release
        if (detIn > envFollower)
            envFollower = attackCoeff  * envFollower + (1.0f - attackCoeff)  * detIn;
        else
            envFollower = releaseCoeff * envFollower + (1.0f - releaseCoeff) * detIn;

        // ── Krzywa GR: stałe 8:1 lub brickwall ──────────────────────────────
        float gr = 1.0f;
        if (envFollower > threshold)
        {
            if (isNukeMode)
            {
                // Brickwall: wyjście klamrowane do threshold
                gr = threshold / envFollower;
            }
            else
            {
                // Ratio 8:1: output_level = threshold + (env - threshold)/8
                const float outLevel = threshold + (envFollower - threshold) / ratioConst;
                gr = outLevel / envFollower;
            }
        }
        // lastGainReduction = SUROWE gr - używane w pętli feedback (SMOOTH).
        // Pętla detekcji musi mieć natychmiastowe sprzężenie (bez wygładzenia),
        // inaczej destabilizuje się ("wycie").
        lastGainReduction = gr;
        if (gr < blockMinGR) blockMinGR = gr;

        // ── GR SMOOTHING dla audio (3-pole LPF 100 Hz) ──────────────────────
        // gr aplikowany do AUDIO jest wygładzony - usuwa ripple 2·f0 który
        // moduluje sygnał i tworzy fałszywe H3/H5 + LF "falowanie".
        // 3-pole (18 dB/okt) - bardzo mocne tłumienie ripple.
        // To NIE jest pętla feedback - tylko mnożnik audio - więc wygładzenie
        // jest bezpieczne (nie destabilizuje).
        grSmoothLPF[0] += grSmoothCoeff * (gr             - grSmoothLPF[0]);
        grSmoothLPF[1] += grSmoothCoeff * (grSmoothLPF[0] - grSmoothLPF[1]);
        grSmoothLPF[2] += grSmoothCoeff * (grSmoothLPF[1] - grSmoothLPF[2]);
        const float grAudio = grSmoothLPF[2];

        // ── Aplikacja GR + parallel saturation per kanał ────────────────────
        //
        // B5 - SATURACJA PROPORCJONALNA DO GR:
        // W prawdziwym 1176, FET-saturator i FET-kompresor to TEN SAM element -
        // saturacja pojawia się TYLKO gdy FET jest w stanie cut-off (czyli gdy
        // sygnał jest kompresowany).  Imitujemy to przez skalowanie głośności
        // harmonik względem aktualnego GR:
        //
        //   gr = 1.0    → satProportion ≈ 0     (brak kompresji = brak saturacji)
        //   gr = 0.7    → satProportion ≈ 0.55  (umiarkowana kompresja, łagodne harmoniki)
        //   gr = 0.4    → satProportion ≈ 0.77  (mocna kompresja, mocne harmoniki)
        //   gr = 0.1    → satProportion ≈ 0.95  (ekstremalna kompresja, pełne harmoniki)
        //
        // Krzywa sqrt: pow(1-gr, 0.5) - szybko rośnie z niewielką kompresją,
        // ale nie eksploduje przy ekstremalnej.  Używamy grAudio (wygładzone)
        // by satProportion też nie miało ripple.
        const float satProportion = std::sqrt (juce::jmax (0.0f, 1.0f - grAudio));

        for (int channel = 0; channel < totalNumInputChannels; ++channel)
        {
            // CLEAN: skompresowany sygnał bez saturacji.  To jest "fundamentalna"
            // odniesienia - drive nigdy jej nie zmienia.
            //
            // B1: audio path czyta próbkę OPÓŹNIONĄ (z lookaheadBuf), podczas
            // gdy 'gr' został policzony z sygnału NIEOPÓŹNIONEGO.  To jest
            // istota lookaheadu - kompresja "wyprzedza" sygnał audio.
            //
            // GR SMOOTHING: używamy grAudio (wygładzone 2-pole LPF 120 Hz)
            // zamiast surowego gr - to eliminuje modulację amplitudową która
            // tworzyła fałszywe H3/H5 i LF "falowanie" przy drive=0.
            const float cleanNow = lookaheadBuf[(size_t) channel][(size_t) laRead]
                                 * inputGain * grAudio;

            // D2 wprowadza 2-próbkowe opóźnienie w torze saturacji (filtr
            // halfband 4-coeff ma grupowe opóźnienie 2 próbek).  Opóźniamy
            // 'clean' o tyle samo by harmonics = saturated - clean było
            // wyrównane fazowo.
            //
            // KLUCZOWE: cleanDelay daje clean = cleanNow[n-2].
            // miniOsUpHist (uh) dostaje cleanNow (NIE opóźnione) - wtedy
            // sampleEven = uh[1] = cleanNow[n-2], co DOKŁADNIE odpowiada
            // wartości 'clean'.  (Wcześniejszy błąd: uh dostawał już opóźnione
            // 'clean', dając sampleEven = cleanNow[n-4] - misalignment 2 próbek
            // względem clean, psujący cancelację LF w harmonics.)
            float* ch_d = cleanDelay[channel];
            const float clean = ch_d[0];          // cleanNow sprzed 2 próbek
            ch_d[0] = ch_d[1];
            ch_d[1] = cleanNow;

            // ── D2: SATURACJA Z MINI-OVERSAMPLINGIEM 2× ─────────────────────
            // Asymptotic saturator + transformer generują harmoniki bardzo
            // wysokiego rzędu.  By uniknąć aliasingu (odbicia od Nyquista),
            // oversamplujemy łańcuch saturacji jeszcze 2×.
            //
            // Upsampling 2× — klasyczny 4-coeff halfband interpolator:
            //   y[2n]   = x[n]                                  (próbka oryginalna)
            //   y[2n+1] = (-x[n-1] + 9·x[n] + 9·x[n+1] - x[n+2]) / 16
            //
            // Filtr wymaga x[n+1], x[n+2] (przyszłe próbki) → utrzymujemy
            // bufor historii i przetwarzamy próbkę opóźnioną o 2.
            //
            // Saturacja działa na obu próbkach (parzysta + interpolowana).
            //
            // Downsampling 2× — uśrednienie z halfband decymacją:
            //   x_out[n] = 0.5·sat_even + 0.25·sat_odd + 0.25·sat_odd_prev
            //   (prosty 3-tap antyaliasingowy decymator, tłumi pasmo > Fs/2)
            float saturated;
            {
                float* uh = miniOsUpHist  [channel];   // [x[n-3],x[n-2],x[n-1],x[n]]
                float* dh = miniOsDownHist[channel];   // historia sat-odd

                // Przesuń historię wejścia, dodaj BIEŻĄCĄ próbkę (cleanNow,
                // nie opóźnione clean - patrz komentarz wyżej o synchronizacji)
                uh[0] = uh[1]; uh[1] = uh[2]; uh[2] = uh[3]; uh[3] = cleanNow;

                // Halfband upsampling - próbkę przetwarzamy z opóźnieniem 2:
                //   "x[n]"   = uh[1]  (centrum filtra)
                //   "x[n-1]" = uh[0], "x[n+1]" = uh[2], "x[n+2]" = uh[3]
                const float sampleEven = uh[1];
                const float sampleOdd  = (-1.0f * uh[0] + 9.0f * uh[1]
                                          + 9.0f * uh[2] - 1.0f * uh[3]) * (1.0f / 16.0f);

                // Saturacja na obu próbkach (przestrzeń 2× oversamplowana)
                const float satEven = processSaturationChain (sampleEven, channel);
                const float satOdd  = processSaturationChain (sampleOdd,  channel);

                // Downsampling - 3-tap halfband decymator antyaliasingowy.
                // Bierzemy satEven (parzysta) + uśrednione sąsiednie odd.
                const float satOddPrev = dh[3];
                dh[0] = dh[1]; dh[1] = dh[2]; dh[2] = dh[3]; dh[3] = satOdd;
                saturated = 0.5f * satEven + 0.25f * satOdd + 0.25f * satOddPrev;
            }

            // HARMONICS: różnica saturowanego od czystego = czysty content harmonik
            // (bez fundamentalnej).  Wszystkie LF artefakty wspólne dla obu
            // sygnałów (modulacja envelope, DC od transformera, etc.) są ZNOSZONE
            // wzajemnie - to eliminuje "garb LF".
            const float harmonics = saturated - clean;

            // OUTPUT: czysty wet + harmoniki skalowane przez drive ORAZ przez
            // satProportion (B5).  Drive = user control (0..1.5), satProportion
            // = automatyczne skalowanie zależne od aktualnego GR.
            float x = clean + harmonics * drive * satProportion;

            // ── BLOKOWANIE DC ────────────────────────────────────────────────
            // Osobny DC blocker (y=x-x1+R·y1) został USUNIĘTY.  Diagnoza:
            // jego biegun przy R≈0.9997 (tuż przy granicy stabilności)
            // wytwarzał długozanikający "ogon" LF i akumulował błędy float32
            // w paśmie 10-50 Hz - to był mierzony przez beta-testera "garb LF"
            // obecny niezależnie od kompresji i drive.
            //
            // Blokowanie DC w pełni zapewnia HPF 6-rzędu 45 Hz w filterChain
            // (poniżej, krok 6) - 6-rzędu Butterworth tłumi składową 0 Hz
            // nieskończenie, bez artefaktu długiego ogona.

            oversampledBlock.setSample (channel, sample, x);
        }

        // B1: przesuń wskaźnik zapisu lookahead buffera (1 próbka osRate)
        lookaheadWrite = (lookaheadWrite + 1) & lookaheadMask;
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

    // ─── 7. Mix + output gain + soft ceiling (D1) ────────────────────────────
    //
    // Tutaj wcześniej pojawiał się phaser: dryBuffer był nieopóźniony,
    // a buffer (wet) wracał z oversamplera spóźniony o 'dryDelayLength' próbek.
    // Mieszanie => filtr grzebieniowy => "phaser" na drum-busie.
    //
    // Naprawa: pobieramy dry z circular buffera, opóźnionego dokładnie tyle
    // ile wynosi latencja oversamplera.
    //
    // D1 - SOFT CEILING (zastąpił poprzedni hard clip):
    // Krzywa:
    //   |x| < 0.85       → liniowo (sygnał przechodzi bez modyfikacji)
    //   0.85 ≤ |x| < ∞   → soft saturation tanh-style, asymptotycznie do ±1.0
    //
    // Zalety vs hard clip (jlimit ±1):
    //   - Brak ostrych kantów w czasie → mniej aliasingu
    //   - Brzmi MUZYKALNIE (jak analogowy "warmth") zamiast cyfrowo
    //   - Sygnały blisko 0 dBFS są łagodnie ograniczane, nie ostro przycinane
    //   - Zachowuje wszystkie wartości w ±1.0 - identyczne bezpieczeństwo
    auto softCeiling = [] (float x) -> float
    {
        constexpr float knee = 0.85f;       // poniżej tego: liniowo
        constexpr float top  = 1.0f;        // asymptota
        const float absX = std::abs (x);
        if (absX < knee)
            return x;
        // Krzywa: y = sign(x) * (knee + (top - knee) * tanh((|x| - knee) / (top - knee)))
        // Dla |x| = knee  → y = knee     (ciągłość C¹ z liniowym fragmentem)
        // Dla |x| → ∞     → |y| → top    (asymptota do ±1.0)
        const float sign   = (x >= 0.0f) ? 1.0f : -1.0f;
        const float excess = absX - knee;
        const float range  = top - knee;
        return sign * (knee + range * std::tanh (excess / range));
    };

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

            // Crossfade fader (1.0 = pełen active, 0.0 = pełen dry)
            const float xf = (crossfadeCounter > 0)
                ? (1.0f - static_cast<float> (crossfadeCounter) / static_cast<float> (crossfadeLength))
                : 1.0f;
            if (crossfadeCounter > 0) --crossfadeCounter;

            for (int ch = 0; ch < chCount; ++ch)
            {
                const float dry    = dryDelayBuf[(size_t) ch][(size_t) readPos];
                const float wet    = buffer.getReadPointer (ch)[s];
                const float active = (dry * (1.0f - m) + wet * m) * o;
                // Crossfade między czystym dry (czyli to co było w bypass) a active
                const float mixed  = dry * (1.0f - xf) + active * xf;
                // Hard clip na 0 dBFS - bezpiecznik przeciw cyfrowemu przesterowaniu
                buffer.getWritePointer (ch)[s] = softCeiling (mixed);
            }
            readPos = (readPos + 1) & dryDelayMask;
        }
    }
    else
    {
        // Brak latencji (ZeroLat IIR może mieć 0) - prosty mix z crossfade
        for (int s = 0; s < numSamples; ++s)
        {
            const float m = mixSmooth      .getNextValue();
            const float o = outputGainSmooth.getNextValue();

            const float xf = (crossfadeCounter > 0)
                ? (1.0f - static_cast<float> (crossfadeCounter) / static_cast<float> (crossfadeLength))
                : 1.0f;
            if (crossfadeCounter > 0) --crossfadeCounter;

            for (int ch = 0; ch < totalNumInputChannels; ++ch)
            {
                const float dry    = dryBuffer.getReadPointer (ch)[s];
                const float wet    = buffer   .getReadPointer (ch)[s];
                const float active = (dry * (1.0f - m) + wet * m) * o;
                const float mixed  = dry * (1.0f - xf) + active * xf;
                buffer.getWritePointer (ch)[s] = softCeiling (mixed);
            }
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