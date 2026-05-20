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
    float processTransformerSaturation (float sample, float amount);
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

    // Flaga do odbudowania oversamplera w prepareToPlay po zmianie parametrów
    std::atomic<bool> oversamplerDirty { false };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ABI1176Processor)
};