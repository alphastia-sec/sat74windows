#pragma once
#include <JuceHeader.h>

using StereoFilter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;

class ABI1176Processor  : public juce::AudioProcessor,
                          public juce::AudioProcessorValueTreeState::Listener
{
public:
    ABI1176Processor();
    ~ABI1176Processor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override { return "DRUM SAT 76"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    // E2: Standardowy bypass JUCE.  Zwracając nasz bypass parameter,
    // pozwalamy DAW zarządzać bypassem przez standardowe API host'a
    // (np. guzik bypass w Cubase mixer strip, automatyzacja z DAW, itp.).
    // Wewnętrzna implementacja w processBlock pozostaje taka sama (czyta
    // bypassParam i robi crossfade) - tylko EKSPONUJEMY tę informację
    // dla host'a.
    juce::AudioProcessorParameter* getBypassParameter() const override
    {
        return apvts.getParameter ("bypass");
    }

    juce::AudioProcessorValueTreeState apvts;
    float getGainReduction() const { return currentGR.load(); }

private:
    float processFETSaturation (float sample, float drive);
    float processTransformerSaturation (float sample, float amount, int channelIdx);
    float processAsymmetricClip (float sample, float bias);

    // D2: pełny łańcuch saturacji (FET → asym → transformer) jako jedna
    // funkcja - wywoływana 2× per próbka wewnątrz mini-oversamplingu.
    float processSaturationChain (float sample, int channelIdx);

    // Inicjalizacja silnika Oversampling
    void updateOversampling();

    // Pomocnicze - rozmiar / reset linii opóźniającej dry
    void resizeDryDelay (int latencySamples, int numChannels);
    void resetDryDelay();

    std::atomic<float>* bypassParam     = nullptr;
    std::atomic<float>* inputGainParam  = nullptr;
    std::atomic<float>* driveParam      = nullptr;
    std::atomic<float>* outputGainParam = nullptr;
    std::atomic<float>* mixParam        = nullptr;
    std::atomic<float>* compModeParam   = nullptr;
    std::atomic<float>* characterParam  = nullptr;

    // Nowe parametry: Zero Latency i FAT
    std::atomic<float>* zeroLatParam    = nullptr;
    std::atomic<float>* fatParam        = nullptr;

    std::atomic<float> currentGR { 0.0f };
    bool wasBypassed = true;
    float lastGainReduction = 1.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> inputGainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmooth;

    // Tor toneshapingu - emulacja transformatora wyjściowego Carnhill (Neve-style):
    //   [0] HPF 15 Hz       -> DC-block po klipperze + lekka rozciągłość basu
    //   [1] Low-shelf 100 Hz, +3 dB -> "warmth" w low-mids, charakter Carnhilla
    //   [2] High-shelf 10 kHz, -1.5 dB -> łagodny roll-off wysokich (Carnhill)
    // Carnhill EQ chain + stromy HPF wyjściowy (LF cleanup).
    //  Sloty 0,1,2 = HPF 6-rzędu (3 kaskadowe biquady Butterworth, 45 Hz):
    //                eliminuje wszystko poniżej fundamentalnej drum-busa
    //                (sub-bass artefakty, modulacyjny LF, leakage) do < -120 dB.
    //  Slot 3 = Low-shelf 120 Hz +1.5 dB (warmth Carnhilla)
    //  Slot 4 = High-shelf 12 kHz -1.2 dB (air roll-off Carnhilla)
    juce::dsp::ProcessorChain<StereoFilter, StereoFilter, StereoFilter,
                              StereoFilter, StereoFilter> filterChain;

    // Obiekt Oversampling
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

    // ─────────────────────────────────────────────────────────────────────
    //  Linia opóźniająca DRY zsynchronizowana z latencją oversamplera.
    //  To rozwiązuje efekt phasera przy dry/wet poniżej 100%.
    // ─────────────────────────────────────────────────────────────────────
    std::vector<std::vector<float>> dryDelayBuf; // [channel][sample]
    int  dryDelayWrite  = 0;
    int  dryDelaySize   = 0;     // pojemność bufora (potęga 2)
    int  dryDelayMask   = 0;
    int  dryDelayLength = 0;     // ile próbek opóźnienia (= latencja OS)

    double currentSampleRate = 44100.0;
    int currentSamplesPerBlock = 512;
    float envFollower  = 0.0f;
    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;

    // ─────────────────────────────────────────────────────────────────────
    //  DETEKTOR — wygładzenie audio-rate tętnienia.
    //
    //  Problem: |sample| dla sinusa f Hz to sygnał DC + harmoniczne parzyste
    //  (2f, 4f, ...).  Bez filtru, envelope follower oscyluje z 2f, co
    //  moduluje gain i wprowadza inter-modulacyjne produkty (artefakty
    //  sub-bass).  Pre-LPF na detektorze (≈ 25 Hz) usuwa tętnienie audio-rate
    //  zostawiając tylko makro-envelope, dokładnie jak fizyczny kondensator
    //  detektora w prawdziwym 1176.
    // ─────────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────────
    //  DETEKTOR — pre-LPF (2-pole RC kaskada).
    //  Wygładza prostowany sygnał przed envelope followerem.  2-pole (12 dB/okt)
    //  tłumi ripple 2·f0 mocniej niż 1-pole - mniej modulacji gain.
    // ─────────────────────────────────────────────────────────────────────
    float detectorLPF[2]     = { 0.0f, 0.0f };
    float detectorLPFCoeff   = 0.0f;

    // ─────────────────────────────────────────────────────────────────────
    //  GR SMOOTHING - wygładzanie gain reduction PRZED aplikacją do audio.
    //
    //  Problem zdiagnozowany na podstawie nagrania użytkownika:
    //  Przy drive=0 (czysty sygnał, x = signal·inputGain·gr) harmoniki
    //  NIEPARZYSTE (H3, H5) "falują", podobnie LF.  Przyczyna:
    //
    //  Envelope follower w trybie attack ma pasmo ~1 kHz, więc 'gr' zawiera
    //  ripple przy 2·f0 (≈ -37 dB).  Mnożenie  signal · gr(t)  to MODULACJA
    //  AMPLITUDOWA → sidebands przy f0±2f0:
    //     f0 + 2f0 = 3f0  → fałszywy H3
    //     f0 - 2f0 = -f0  → mirror do LF
    //  Sidebands interferują z dryfem fazy → "falowanie".
    //
    //  Rozwiązanie: gr aplikowany do audio jest wygładzany 2-pole LPF 120 Hz.
    //  Ripple 2·f0 (≥520 Hz) jest tłumiony o ~26 dB, a prawdziwe zmiany gr
    //  (< 120 Hz, bo envelope follower) przechodzą swobodnie - więc attack
    //  kompresji NIE jest spowolniony.
    //
    //  Detektor (envelope follower) pozostaje szybki - wygładzamy TYLKO
    //  mnożnik audio, nie pętlę detekcji.
    // ─────────────────────────────────────────────────────────────────────
    float grSmoothLPF[3]   = { 1.0f, 1.0f, 1.0f };   // 3-pole kaskada
    float grSmoothCoeff    = 0.0f;

    // ─────────────────────────────────────────────────────────────────────
    //  A1: SIDE-CHAIN HPF na detektorze (juce::dsp::IIR).
    //
    //  Klasyczna technika w 1176-style: detektor "nie widzi" basu (kompresor
    //  ignoruje LF), więc nie pompuje na bębnie kicka ani basie.  To również
    //  eliminuje sidebands modulacyjne w paśmie LF - bo gain reduction
    //  zmienia się TYLKO w odpowiedzi na sygnał > 80 Hz.
    //
    //  Implementacja: juce::dsp::IIR::Filter zamiast ręczny biquad.  Powody:
    //   - Stabilniejsza numerycznie (Direct Form II Transposed)
    //   - Auto-handling denormali (JUCE wewnętrznie)
    //   - Mniej kodu, mniej okazji do bugów
    //   - Standardowy reset() i prepare()
    //
    //  Side-chain HPF jest MONO (det jest skalarem), więc używamy zwykłego
    //  IIR::Filter, nie ProcessorDuplicator.
    // ─────────────────────────────────────────────────────────────────────
    juce::dsp::IIR::Filter<float> sidechainHPF;

    // ─────────────────────────────────────────────────────────────────────
    //  D2: MINI-OVERSAMPLING 2× wewnątrz saturatorów.
    //
    //  Problem: asymptotic saturator + transformer generują harmoniki bardzo
    //  wysokiego rzędu (H19+ dla sinusa 5 kHz = 95 kHz).  Przy 192 kHz osRate
    //  (Nyquist 96 kHz) najwyższe harmoniki odbijają się = aliasing.
    //
    //  Rozwiązanie: wewnątrz toru saturacji oversamplujemy jeszcze 2×.
    //  Upsampling: 4-tap halfband FIR (windowed sinc) - daje ~60 dB tłumienia.
    //  Downsampling: ten sam halfband (decymacja po 2).
    //
    //  State: 4 ostatnie próbki wejścia per kanał (do interpolacji halfband)
    //  + 4 ostatnie próbki wyjścia per kanał (do filtra decymacji).
    // ─────────────────────────────────────────────────────────────────────
    float miniOsUpHist[2][4]   = { { 0.0f, 0.0f, 0.0f, 0.0f },
                                   { 0.0f, 0.0f, 0.0f, 0.0f } };
    float miniOsDownHist[2][4] = { { 0.0f, 0.0f, 0.0f, 0.0f },
                                   { 0.0f, 0.0f, 0.0f, 0.0f } };
    // 2-próbkowa linia opóźniająca dla 'clean' - synchronizuje fundamentalną
    // z opóźnieniem grupowym mini-oversamplingu (halfband filter latency).
    float cleanDelay[2][2]     = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };

    // ─────────────────────────────────────────────────────────────────────
    //  B1: LOOKAHEAD
    //
    //  Sygnał audio jest opóźniony o ~1.5 ms względem sygnału detekcji.
    //  Dzięki temu detektor "widzi przyszłość" - kompresor zaczyna redukcję
    //  ZANIM transient dotrze do toru audio.  Efekt: zero overshoot na
    //  ostrych transientach (kick, snare crack).
    //
    //  Linia opóźniająca działa w przestrzeni OVERSAMPLOWANEJ (osRate),
    //  per kanał.  Długość = round(1.5 ms × osRate), zaokrąglona w górę do
    //  potęgi 2 dla maskowania indeksu.
    //
    //  Dodatkowa latencja jest zgłaszana hostowi przez setLatencySamples
    //  (zsumowana z latencją oversamplera).
    // ─────────────────────────────────────────────────────────────────────
    std::vector<std::vector<float>> lookaheadBuf;   // [channel][sample]
    int lookaheadWrite  = 0;
    int lookaheadSize   = 0;       // pojemność (potęga 2)
    int lookaheadMask   = 0;
    int lookaheadLength = 0;       // ile próbek opóźnienia (w osRate)

    // ─────────────────────────────────────────────────────────────────────
    //  TRANSFORMER (Carnhill model) - A2: nowy fizyczny model B-H.
    //  Wcześniejsze pola transformerDCEst/Coeff zostały usunięte - nowy
    //  model nie produkuje DC offsetu (asymetryczny Padé approximant
    //  z różną krzywizną w gałęziach +/-), więc nie potrzebujemy LPF
    //  do estymacji DC.  Patrz processTransformerSaturation.
    // ─────────────────────────────────────────────────────────────────────

    // ─────────────────────────────────────────────────────────────────────
    //  BYPASS CROSSFADE
    //
    //  Po wyjściu z bypassu, plugin ma "świeże" stany (filtry, DC blocker,
    //  detector, oversampler) - na pierwszych próbkach mogą one wytworzyć
    //  transienty (impulse response filtrów, różnica dry/wet, attack
    //  detektora przy zerowym env).  Crossfade między dry a active sygnałem
    //  na 'crossfadeLength' próbek (≈ 30 ms) całkowicie maskuje te transienty.
    // ─────────────────────────────────────────────────────────────────────
    int crossfadeCounter = 0;
    int crossfadeLength  = 0;

    // ─────────────────────────────────────────────────────────────────────
    //  DC BLOCKERY na wyjściu każdej asymetrycznej nieliniowości.
    //  Asymetria wprowadza powolny DC drift który tworzy infrasonic noise.
    //  Pojedyncza linia opóźniająca-prostowanie: y[n] = x[n] - x[n-1] + R*y[n-1]
    // ─────────────────────────────────────────────────────────────────────
    // DC blocker usunięty - blokowanie DC zapewnia HPF 6-rzędu 45 Hz
    // w filterChain.  Osobny 1-pole DC blocker wytwarzał LF "ogon" artefakt.

    // Flaga do odbudowania oversamplera w prepareToPlay po zmianie parametrów
    std::atomic<bool> oversamplerDirty { false };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ABI1176Processor)
};