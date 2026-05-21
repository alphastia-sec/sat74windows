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

    // Detector LPF — usuwa tętnienie audio-rate z prostowanego sygnału,
    // dokładnie jak fizyczny kondensator detektora w prawdziwym 1176.
    // 25 Hz cut-off w przestrzeni oversamplowanej: powyżej tej częstotliwości
    // prostowany sygnał jest tłumiony o > 20 dB, więc envelope ma tylko makro-trend.
    {
        const float fc = 25.0f;
        const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
        const float dt = 1.0f / static_cast<float> (osRate);
        detectorLPFCoeff = dt / (rc + dt);     // jednoczas RC LPF
        detectorLPF      = 0.0f;
    }

    // Side-chain HPF biquad 2-pole (80 Hz, Q=0.707) - RBJ formula.
    // Tłumi bas w torze DETEKTORA (audio path nie tknięty), eliminując
    // pompowanie basowe i sidebands modulacyjne w paśmie LF.
    {
        const float fc    = 80.0f;
        const float Q     = 0.707f;
        const float w0    = 2.0f * juce::MathConstants<float>::pi * fc / static_cast<float> (osRate);
        const float cosw  = std::cos (w0);
        const float sinw  = std::sin (w0);
        const float alpha = sinw / (2.0f * Q);
        const float a0    = 1.0f + alpha;
        scHpfB0 =  (1.0f + cosw) / 2.0f / a0;
        scHpfB1 = -(1.0f + cosw) / a0;
        scHpfB2 =  (1.0f + cosw) / 2.0f / a0;
        scHpfA1 = -2.0f * cosw / a0;
        scHpfA2 =  (1.0f - alpha) / a0;
        scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
    }

    // Transformer DC compensation LPF (~50 Hz) - dla aktywnej kompensacji
    // DC offsetu wewnątrz Carnhill x²-saturatora.  Patrz komentarz w
    // processTransformerSaturation.
    {
        const float fc = 50.0f;
        const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
        const float dt = 1.0f / static_cast<float> (osRate);
        transformerDCCoeff = dt / (rc + dt);
        transformerDCEst[0] = transformerDCEst[1] = 0.0f;
    }

    // DC blocker (~10 Hz pole) w przestrzeni oversamplowanej.
    // y[n] = x[n] - x[n-1] + R*y[n-1]
    // R = 1 - 2*pi*fc/fs    (dla fc << fs)
    {
        const float fc = 10.0f;
        dcBlockR  = 1.0f - (2.0f * juce::MathConstants<float>::pi * fc
                            / static_cast<float> (osRate));
        dcBlockX1[0] = dcBlockX1[1] = 0.0f;
        dcBlockY1[0] = dcBlockY1[1] = 0.0f;
    }

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = osRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock * oversampler->getOversamplingFactor());
    spec.numChannels      = 2;

    // Tor wyjściowy - emulacja transformatora Carnhill (Neve-style):
    //  - 1) HPF 30 Hz, Q=0.707 - 2-pole / 12 dB/oktawa - skutecznie odcina
    //       infrasonic artefakty z modulacji envelope kompresora (dawniej 1-pole
    //       6 dB/okt nie tłumił wystarczająco pasma 30-100 Hz, gdzie generowały
    //       się sidebands modulacyjne).
    //  - 2) Low-shelf 120 Hz, +1.5 dB, Q=0.7 - delikatna "warmth" Carnhilla
    //  - 3) High-shelf 12 kHz, -1.2 dB, Q=0.7 - łagodny "air" roll-off
    filterChain.get<0>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass  (osRate, 30.0f, 0.707f);
    filterChain.get<1>().state = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (osRate, 120.0f, 0.7f, juce::Decibels::decibelsToGain (1.5f));
    filterChain.get<2>().state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (osRate, 12000.0f, 0.7f, juce::Decibels::decibelsToGain (-1.2f));
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
    const float gain      = 0.80f;        // stały gain (bo drive nie skaluje saturacji)
    const float satCoeff  = 0.5f;

    const float x = sample * gain;

    float y;
    if (x >= 0.0f)  y = x / (1.0f + x * satCoeff);
    else            y = x / (1.0f - x * satCoeff);

    return y / gain;     // unity gain dla małych sygnałów
}

float ABI1176Processor::processTransformerSaturation (float sample, float amount, int channelIdx)
{
    // Carnhill output transformer model - JEDYNE ŹRÓDŁO 2nd harmonic content
    // w całym torze.  Wszystkie wcześniejsze etapy są SYMETRYCZNE i produkują
    // tylko nieparzyste harmoniki - tutaj dodajemy delikatne parzyste.
    //
    // KRZYWA Z AKTYWNĄ KOMPENSACJĄ DC:
    //   raw  = α·x²        - produkuje 2nd harmonic + DC (zmienny w czasie!)
    //   x²_lp = LPF (raw)  - estymata średniej składowej DC (~50 Hz cut-off)
    //   y    = x + raw - x²_lp   - 2nd harmonic ZACHOWANE, DC USUNIĘTE
    //
    // Dlaczego to lepsze niż DC blocker w głównym torze:
    //   - DC blocker za saturacją (10 Hz HPF) usuwa średnią, ale tworzy
    //     "infrasonic ripple" gdy DC component zmienia się DYNAMICZNIE
    //     (np. przy attack/release).  Ten ripple → garb LF na FFT.
    //   - Lokalna kompensacja DC W FUNKCJI usuwa offset zanim wpłynie na DC
    //     blocker, więc DC blocker widzi sygnał już DC-balanced.
    //
    // Per-channel state (channelIdx 0=L, 1=R) - każdy kanał ma własny estymator.
    //
    // alpha = 0.025·amount generuje ≈ -52 dB 2nd harmonic dla sinusa 0.5 amp
    // (zgodnie z pomiarami Carnhilla VTB1).

    const int    ch    = juce::jlimit (0, 1, channelIdx);
    const float alpha  = 0.025f * amount;
    const float x2     = sample * sample;        // 2nd-order content (z DC = <x²>)

    // Estymata średniej x² (lokalna kompensacja DC) per kanał
    transformerDCEst[ch] += transformerDCCoeff * (x2 - transformerDCEst[ch]);
    const float x2_ac = x2 - transformerDCEst[ch];  // x² bez DC component

    // y = x + α·(x² - <x²>)  → 2nd harmonic obecne, DC = 0
    const float x_with_2nd = sample + alpha * x2_ac;

    // 2) Łagodna saturacja core - tylko gdy sygnał osiąga thresh
    const float thresh = 0.7f;
    const float absA   = std::abs (x_with_2nd);

    if (absA < thresh)
        return x_with_2nd;

    // Soft kompresja powyżej threshold (symetryczna - tylko nieparzyste)
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
        filterChain.get<0>().state = juce::dsp::IIR::Coefficients<float>::makeHighPass  (osRate, 30.0f, 0.707f);
        filterChain.get<1>().state = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (osRate, 120.0f, 0.7f, juce::Decibels::decibelsToGain (1.5f));
        filterChain.get<2>().state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (osRate, 12000.0f, 0.7f, juce::Decibels::decibelsToGain (-1.2f));
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
            detectorLPF      = 0.0f;
        }
        // Side-chain HPF - przelicz dla nowego osRate
        {
            const float fc    = 80.0f;
            const float Q     = 0.707f;
            const float w0    = 2.0f * juce::MathConstants<float>::pi * fc / static_cast<float> (osRate);
            const float cosw  = std::cos (w0);
            const float sinw  = std::sin (w0);
            const float alpha = sinw / (2.0f * Q);
            const float a0    = 1.0f + alpha;
            scHpfB0 =  (1.0f + cosw) / 2.0f / a0;
            scHpfB1 = -(1.0f + cosw) / a0;
            scHpfB2 =  (1.0f + cosw) / 2.0f / a0;
            scHpfA1 = -2.0f * cosw / a0;
            scHpfA2 =  (1.0f - alpha) / a0;
            scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
        }
        {
            const float fc = 50.0f;
            const float rc = 1.0f / (2.0f * juce::MathConstants<float>::pi * fc);
            const float dt = 1.0f / static_cast<float> (osRate);
            transformerDCCoeff = dt / (rc + dt);
            transformerDCEst[0] = transformerDCEst[1] = 0.0f;
        }
        {
            const float fc = 10.0f;
            dcBlockR  = 1.0f - (2.0f * juce::MathConstants<float>::pi * fc
                                / static_cast<float> (osRate));
            dcBlockX1[0] = dcBlockX1[1] = 0.0f;
            dcBlockY1[0] = dcBlockY1[1] = 0.0f;
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
        // Reset detektora i DC blockera - aby nie mieć "duchów" z poprzednio
        // skompresowanego sygnału, gdy plugin wraca z bypassu
        detectorLPF       = 0.0f;
        transformerDCEst[0] = transformerDCEst[1] = 0.0f;
        scHpfX1 = scHpfX2 = scHpfY1 = scHpfY2 = 0.0f;
        dcBlockX1[0] = dcBlockX1[1] = 0.0f;
        dcBlockY1[0] = dcBlockY1[1] = 0.0f;
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

        // Średnia mono detektora (suma L+R) - klasyczny stereo-linked 1176
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
        {
            const float scIn  = det;
            const float scOut = scHpfB0 * scIn
                              + scHpfB1 * scHpfX1
                              + scHpfB2 * scHpfX2
                              - scHpfA1 * scHpfY1
                              - scHpfA2 * scHpfY2;
            scHpfX2 = scHpfX1; scHpfX1 = scIn;
            scHpfY2 = scHpfY1; scHpfY1 = scOut;
            det = std::abs (scOut);   // ponowne prostowanie po HPF
        }

        // ── Pre-detector LPF (~25 Hz) ───────────────────────────────────────
        // Usuwa audio-rate ripple z prostowanego sygnału, dokładnie jak fizyczny
        // kondensator detektora w prawdziwym 1176.  Bez tego envelope follower
        // oscyluje z 2f0 i moduluje gain w paśmie audio.
        detectorLPF += detectorLPFCoeff * (det - detectorLPF);
        const float detSmoothed = detectorLPF;

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
        lastGainReduction = gr;
        if (gr < blockMinGR) blockMinGR = gr;

        // ── Aplikacja GR + parallel saturation per kanał ────────────────────
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
        {
            // CLEAN: skompresowany sygnał bez saturacji.  To jest "fundamentalna"
            // odniesienia - drive nigdy jej nie zmienia.
            const float clean = oversampledBlock.getSample (channel, sample)
                              * inputGain * gr;

            // SATURATED: pełny tor saturacji (FET → asym → transformer Carnhill).
            float saturated = clean;
            saturated = processFETSaturation         (saturated, 1.0f);   // pełny drive=1
            saturated = processAsymmetricClip        (saturated, 0.5f);
            saturated = processTransformerSaturation (saturated, 0.3f, channel);

            // HARMONICS: różnica saturowanego od czystego = czysty content harmonik
            // (bez fundamentalnej).  Wszystkie LF artefakty wspólne dla obu
            // sygnałów (modulacja envelope, DC od transformera, etc.) są ZNOSZONE
            // wzajemnie - to eliminuje "garb LF".
            const float harmonics = saturated - clean;

            // OUTPUT: czysty wet + skalowane harmoniki
            float x = clean + harmonics * drive;

            // ── DC BLOCKER (lekkie zabezpieczenie) ───────────────────────────
            // Po parallel saturation DC offset jest już mały (różnica saturated-clean
            // anuluje DC offset transformera), ale zostawiamy DC blocker jako
            // dodatkowe zabezpieczenie przed dryftem.
            const int dcCh   = juce::jlimit (0, 1, channel);
            const float xIn  = x;
            const float yOut = xIn - dcBlockX1[dcCh] + dcBlockR * dcBlockY1[dcCh];
            dcBlockX1[dcCh] = xIn;
            dcBlockY1[dcCh] = yOut;
            x = yOut;

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
    // HARD CLIP na 0 dBFS:
    // Po zastosowaniu output_gain (które może być +24 dB) klamruje sygnał do
    // ±1.0.  Tym samym OUTPUT może podbić wet sygnał, ale nigdy nie przekroczy
    // poziomu 0 dBFS - chroni odbiorcę przed cyfrowym przesterowaniem.
    //
    // Hard clip jest aplikowany w PRZESTRZENI HOST RATE (po downsamplingu),
    // co oznacza że może produkować aliasing dla bardzo gorących sygnałów.
    // W praktyce sygnał wchodzący do hard clipa jest już bardzo blisko 0 dBFS
    // (zazwyczaj kompresor go ogranicza), więc aliasing jest minimalny.
    auto hardClip = [] (float x) -> float
    {
        return juce::jlimit (-1.0f, 1.0f, x);
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
                buffer.getWritePointer (ch)[s] = hardClip (mixed);
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
                buffer.getWritePointer (ch)[s] = hardClip (mixed);
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