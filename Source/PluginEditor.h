#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// UI v10: 1600x250. TEAC-style VU meter — the arc, scale and needle now fill the
// face properly: pivot deep below the face for a classic flat-VU look, long thin
// needle resting at the right "0 dB GR" mark, red dB numerals (0, 5, 10, 15, 20)
// sitting just outside the arc, and a small dark boss slot below the boss.
// Knobs unchanged.  BYPASS toggle in the top-left, four labelled toggles on the
// bottom (NUKE, SMOOTH, ZERO LAT, FAT).
class DotOnlyLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    DotOnlyLookAndFeel();

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider& slider) override;

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics& g,
                         juce::TextButton& button,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
};

class ABI1176Editor : public juce::AudioProcessorEditor,
                      private juce::Timer
{
public:
    ABI1176Editor (ABI1176Processor&, juce::AudioProcessorValueTreeState&);
    ~ABI1176Editor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;

private:
    void timerCallback() override;

    void drawVUMeter (juce::Graphics& g);
    void drawScrew   (juce::Graphics& g, float cx, float cy, float r);

    float needleAngle = 0.78f;
    juce::Image bgImage;

    ABI1176Processor&                   audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;

    DotOnlyLookAndFeel dotLook;

    juce::Slider inputKnob, driveKnob, outputKnob, mixKnob;

    juce::TextButton btnBypass, btnNuke, btnMode, btnZeroLat, btnFat;

    // Label captions placed beneath each toggle.
    juce::Label lblBypass, lblNuke, lblMode, lblZeroLat, lblFat;

    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAtt    = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAtt> attInput, attDrive, attOutput, attMix;
    std::unique_ptr<BtnAtt>    attBypass, attNuke, attMode, attZeroLat, attFat;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ABI1176Editor)
};