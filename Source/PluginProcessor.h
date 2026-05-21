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

    juce::AudioProcessorValueTreeState apvts;
    float getGainReduction() const { return currentGR.load(); }

private:
    float processFETSaturation (float sample, float drive);
    float processTransformerSaturation (float sample, float amount, int channelIdx);
    float processAsymmetricClip (float sample, float bias);

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
    juce::dsp::ProcessorChain<StereoFilter, StereoFilter, StereoFilter> filterChain;

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
    float detectorLPF        = 0.0f;
    float detectorLPFCoeff   = 0.0f;

    // ─────────────────────────────────────────────────────────────────────
    //  SIDE-CHAIN HPF na detektorze.
    //
    //  Klasyczna technika w 1176-style: detektor "nie widzi" basu (kompresor
    //  ignoruje LF), więc nie pompuje na bębnie kicka ani basie.  To również
    //  eliminuje sidebands modulacyjne w paśmie LF - bo gain reduction
    //  zmienia się TYLKO w odpowiedzi na sygnał > 80 Hz, niskie pasma
    //  fundamentalnej (np. 60 Hz) nie wywołują modulacji.
    //
    //  Biquad HPF 2-pole, fc ≈ 80 Hz, Q ≈ 0.707.
    //  Filtr działa TYLKO na sygnale detekcji, NIE na audio path.
    // ─────────────────────────────────────────────────────────────────────
    float scHpfB0 = 1.0f, scHpfB1 = 0.0f, scHpfB2 = 0.0f;
    float scHpfA1 = 0.0f, scHpfA2 = 0.0f;
    float scHpfX1 = 0.0f, scHpfX2 = 0.0f;
    float scHpfY1 = 0.0f, scHpfY2 = 0.0f;

    // ─────────────────────────────────────────────────────────────────────
    //  TRANSFORMER DC COMPENSATION (per kanał)
    //  Krzywa Carnhilla y = x + α·x² produkuje 2nd harmonic + ZMIENNY W CZASIE
    //  DC offset (proporcjonalny do <x²> czyli mocy sygnału).  Klasyczny DC
    //  blocker (HPF 10 Hz) usuwa średnią, ale gdy DC oscyluje dynamicznie
    //  (np. przy attack/release kompresora), filtr produkuje "infrasonic
    //  ripple" widoczny jako garb LF na FFT.
    //
    //  Rozwiązanie: lokalnie śledzimy <x²> per kanał przez LPF i odejmujemy
    //  go od x² zanim trafi do reszty toru.  DC blocker dalej w torze widzi
    //  sygnał już DC-balanced, więc nie produkuje ripple.
    //
    //  cut-off ~50 Hz: szybkie dostrojenie do zmian dynamicznych, ale dość
    //  wolne by nie tłumić rzeczywistego 2nd harmonic ≥ 100 Hz.
    // ─────────────────────────────────────────────────────────────────────
    float transformerDCEst[2] = { 0.0f, 0.0f };
    float transformerDCCoeff  = 0.0f;

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
    float dcBlockX1[2] = { 0.0f, 0.0f };
    float dcBlockY1[2] = { 0.0f, 0.0f };
    float dcBlockR     = 0.0f;

    // Flaga do odbudowania oversamplera w prepareToPlay po zmianie parametrów
    std::atomic<bool> oversamplerDirty { false };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ABI1176Processor)
};