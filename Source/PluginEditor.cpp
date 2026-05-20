#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr int editorW = 1600;
    constexpr int editorH = 250;

    constexpr juce::uint32 switchOn    = 0xff111111u;
    constexpr juce::uint32 meterInk    = 0xff111111u;
    constexpr juce::uint32 meterRed    = 0xffad1f1fu;
}

DotOnlyLookAndFeel::DotOnlyLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId,       juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
    setColour (juce::Slider::thumbColourId,             juce::Colours::transparentBlack);
    setColour (juce::Slider::rotarySliderFillColourId,  juce::Colours::transparentBlack);
    setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colours::transparentBlack);
}

void DotOnlyLookAndFeel::drawRotarySlider (juce::Graphics& g,
                                           int x, int y, int width, int height,
                                           float sliderPos,
                                           float startAngle,
                                           float endAngle,
                                           juce::Slider&)
{
    auto area = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);

    const float cx = area.getCentreX();
    const float cy = area.getCentreY() + 8.0f;
    
    juce::ignoreUnused (startAngle, endAngle);
    constexpr float visualStartAngle = -juce::MathConstants<float>::pi * 0.80f;
    constexpr float visualEndAngle   =  juce::MathConstants<float>::pi * 0.80f;
    const float angle = visualStartAngle + sliderPos * (visualEndAngle - visualStartAngle);

    const float knobR = juce::jmin (area.getWidth(), area.getHeight()) * 0.25f;

    // Red indicator LINE pointing outward from the knob centre to its current angle.
    const float lineThickness = juce::jmax (1.5f, knobR * 0.085f);
    const float lineInnerR    = knobR * 0.28f;
    const float lineOuterR    = knobR * 0.82f - lineThickness * 0.5f;

    const float sinA = std::sin (angle);
    const float cosA = std::cos (angle);

    const float x1 = cx + sinA * lineInnerR;
    const float y1 = cy - cosA * lineInnerR;
    const float x2 = cx + sinA * lineOuterR;
    const float y2 = cy - cosA * lineOuterR;

    juce::Path indicator;
    indicator.startNewSubPath (x1, y1);
    indicator.lineTo          (x2, y2);

    const juce::PathStrokeType stroke (lineThickness,
                                       juce::PathStrokeType::JointStyle::curved,
                                       juce::PathStrokeType::EndCapStyle::butt);

    // Soft drop shadow beneath the indicator.
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.strokePath (indicator,
                  stroke,
                  juce::AffineTransform::translation (1.0f, 1.5f));

    // The red value indicator itself.
    g.setColour (juce::Colours::red);
    g.strokePath (indicator, stroke);
}

void DotOnlyLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                               juce::Button& button,
                                               const juce::Colour&,
                                               bool isHighlighted,
                                               bool isDown)
{
    auto b = button.getLocalBounds().toFloat().reduced (2.0f);
    const bool on = button.getToggleState();

    g.setColour (juce::Colours::black.withAlpha (0.50f));
    g.fillRoundedRectangle (b.translated (0.0f, 2.0f), b.getHeight() * 0.5f);

    auto fill = on ? juce::Colour (switchOn) : juce::Colour (0xffe9e9e9);
    if (isHighlighted || isDown)
        fill = fill.brighter (0.10f);

    g.setColour (fill);
    g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (b, b.getHeight() * 0.5f, 1.6f);
}

void DotOnlyLookAndFeel::drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool)
{
}

ABI1176Editor::ABI1176Editor (ABI1176Processor& p,
                              juce::AudioProcessorValueTreeState& s)
    : juce::AudioProcessorEditor (&p), audioProcessor (p), apvts (s)
{
    bgImage = juce::ImageCache::getFromMemory (BinaryData::background_png, BinaryData::background_pngSize);

    setLookAndFeel (&dotLook);

    auto prepareDotSlider = [this] (juce::Slider& slider)
    {
        slider.setLookAndFeel (&dotLook);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (-juce::MathConstants<float>::pi * 0.80f,
                                      juce::MathConstants<float>::pi * 0.80f,
                                      true);
        slider.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        slider.setPopupDisplayEnabled (false, false, nullptr);
        addAndMakeVisible (slider);
    };

    prepareDotSlider (inputKnob);
    prepareDotSlider (driveKnob);
    prepareDotSlider (outputKnob);
    prepareDotSlider (mixKnob);

    auto prepareDotButton = [this] (juce::TextButton& button)
    {
        button.setButtonText ({});
        button.setClickingTogglesState (true);
        button.setLookAndFeel (&dotLook);
        button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        addAndMakeVisible (button);
    };

    prepareDotButton (btnBypass);
    prepareDotButton (btnNuke);
    prepareDotButton (btnMode);
    prepareDotButton (btnZeroLat);
    prepareDotButton (btnFat);

    auto prepareLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setColour (juce::Label::textColourId, juce::Colour (0xff111111));
        l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        l.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    };

    prepareLabel (lblBypass,  "BYPASS");
    prepareLabel (lblNuke,    "NUKE");
    prepareLabel (lblMode,    "SMOOTH");
    prepareLabel (lblZeroLat, "ZERO LAT");
    prepareLabel (lblFat,     "FAT");

    attInput   = std::make_unique<SliderAtt> (apvts, "inputGain",  inputKnob);
    attDrive   = std::make_unique<SliderAtt> (apvts, "drive",      driveKnob);
    attOutput  = std::make_unique<SliderAtt> (apvts, "outputGain", outputKnob);
    attMix     = std::make_unique<SliderAtt> (apvts, "mix",        mixKnob);

    attBypass  = std::make_unique<BtnAtt> (apvts, "bypass",    btnBypass);
    attNuke    = std::make_unique<BtnAtt> (apvts, "character", btnNuke);
    attMode    = std::make_unique<BtnAtt> (apvts, "compMode",  btnMode);
    attZeroLat = std::make_unique<BtnAtt> (apvts, "zeroLat",   btnZeroLat);
    attFat     = std::make_unique<BtnAtt> (apvts, "fat",       btnFat);

    setResizable (false, false);
    setSize (editorW, editorH);
    startTimerHz (30);
}

ABI1176Editor::~ABI1176Editor()
{
    inputKnob .setLookAndFeel (nullptr);
    driveKnob .setLookAndFeel (nullptr);
    outputKnob.setLookAndFeel (nullptr);
    mixKnob   .setLookAndFeel (nullptr);

    btnBypass .setLookAndFeel (nullptr);
    btnNuke   .setLookAndFeel (nullptr);
    btnMode   .setLookAndFeel (nullptr);
    btnZeroLat.setLookAndFeel (nullptr);
    btnFat    .setLookAndFeel (nullptr);

    setLookAndFeel (nullptr);
    stopTimer();
}

void ABI1176Editor::timerCallback()
{
    constexpr float needleAngleAtZeroGR = +0.78f;
    constexpr float needleAngleAtFullGR = -0.78f;

    const float grDB   = audioProcessor.getGainReduction();
    const float grNorm = juce::jlimit (0.0f, 1.0f, -grDB / 20.0f);
    const float target = needleAngleAtZeroGR
                       + grNorm * (needleAngleAtFullGR - needleAngleAtZeroGR);

    needleAngle += (target - needleAngle) * 0.22f;
    repaint();
}

void ABI1176Editor::drawScrew (juce::Graphics& g, float cx, float cy, float r)
{
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.fillEllipse (cx - r + 1.0f, cy - r + 1.5f, r * 2.0f, r * 2.0f);

    juce::ColourGradient grad (juce::Colour (0xffd8d8d8), cx - r, cy - r,
                               juce::Colour (0xff444444), cx + r, cy + r, true);
    g.setGradientFill (grad);
    g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);

    g.setColour (juce::Colours::black.withAlpha (0.80f));
    g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.0f);

    juce::Path slot;
    slot.addRectangle (-r * 0.70f, -0.75f, r * 1.40f, 1.5f);
    g.fillPath (slot, juce::AffineTransform::rotation ((cx + cy) * 0.017f).translated (cx, cy));
}

void ABI1176Editor::drawVUMeter (juce::Graphics& g)
{
    const float editorHeight  = (float) getHeight();
    const float meterHeight   = editorHeight * 0.60f;
    const float meterTopY     = editorHeight * 0.20f;
    const float meterWidth    = meterHeight * (670.0f / 230.0f);
    const float housingX      = (float) getWidth() - meterWidth - 25.0f;

    juce::Rectangle<float> housing (housingX, meterTopY, meterWidth, meterHeight);
    const auto face = housing.reduced (16.0f, 14.0f);

    const float faceH = face.getHeight();
    const float faceW = face.getWidth();
    const float pivotX = face.getCentreX();
    const float pivotY = face.getBottom() + faceH * 0.80f;
    const float rOuter = faceH * 1.80f;

    const float angMin = -0.78f;
    const float angMax =  0.78f;

    const float tickMajorLen = juce::jmin (faceH * 0.10f, 14.0f);
    const float tickMinorLen = juce::jmin (faceH * 0.05f, 7.0f);

    for (int i = 0; i <= 20; ++i)
    {
        const float t = (float) i / 20.0f;
        const float a = juce::jmap (t, 0.0f, 1.0f, angMin, angMax);
        const bool major = (i % 5) == 0;

        const float r1 = rOuter - (major ? tickMajorLen : tickMinorLen);
        const float r2 = rOuter;

        g.setColour (juce::Colour (meterInk));
        g.drawLine (pivotX + std::sin (a) * r1,
                    pivotY - std::cos (a) * r1,
                    pivotX + std::sin (a) * r2,
                    pivotY - std::cos (a) * r2,
                    major ? 2.0f : 0.9f);
    }

    juce::Path arc;
    arc.addCentredArc (pivotX, pivotY, rOuter, rOuter, 0.0f, angMin, angMax, true);
    g.setColour (juce::Colour (meterInk));
    g.strokePath (arc, juce::PathStrokeType (1.6f));

    {
        const int dbValues[] = { 20, 15, 10, 5, 0 };
        const float labelRadius = rOuter + juce::jmin (faceH * 0.085f, 12.0f);
        const float fontH       = juce::jlimit (10.0f, 14.0f, faceH * 0.115f);

        g.setColour (juce::Colour (meterRed));
        g.setFont (juce::Font (juce::FontOptions (fontH, juce::Font::bold)));

        for (int i = 0; i < 5; ++i)
        {
            const float t = (float) i / 4.0f;
            const float a = juce::jmap (t, 0.0f, 1.0f, angMin, angMax);

            const float lx = pivotX + std::sin (a) * labelRadius;
            const float ly = pivotY - std::cos (a) * labelRadius;

            const float boxW = 26.0f;
            const float boxH = fontH * 1.25f;

            g.drawText (juce::String (dbValues[i]),
                        juce::Rectangle<float> (lx - boxW * 0.5f, ly - boxH * 0.5f, boxW, boxH),
                        juce::Justification::centred,
                        false);
        }
    }

    {
        const float capH = juce::jlimit (8.0f, 11.0f, faceH * 0.085f);
        g.setColour (juce::Colour (meterInk).withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (capH, juce::Font::bold)));
        g.drawText ("GAIN REDUCTION (dB)",
                    juce::Rectangle<float> (face.getX(),
                                            face.getY() + 16.0f,
                                            face.getWidth(),
                                            capH * 1.4f),
                    juce::Justification::centred,
                    false);
    }

    const float bossR = juce::jmin (faceH * 0.06f, 8.0f);
    const float bossY = face.getBottom() - bossR * 1.8f;

    const float a = needleAngle;
    const float sinA = std::sin (a);
    const float cosA = std::cos (a);

    const float needleTipR  = rOuter - 3.0f;
    const float pivotToBoss = pivotY - bossY;
    const float needleBaseR = pivotToBoss - bossR * 0.20f;

    const float nx1 = pivotX + sinA * needleBaseR;
    const float ny1 = pivotY - cosA * needleBaseR;
    const float nx2 = pivotX + sinA * needleTipR;
    const float ny2 = pivotY - cosA * needleTipR;

    g.setColour (juce::Colours::black.withAlpha (0.30f));
    g.drawLine (nx1 + 1.0f, ny1 + 1.5f, nx2 + 1.0f, ny2 + 1.5f, 2.4f);

    juce::Path needlePath;
    needlePath.startNewSubPath (nx1, ny1);
    needlePath.lineTo          (nx2, ny2);
    g.setColour (juce::Colour (0xff0a0a0a));
    g.strokePath (needlePath,
                  juce::PathStrokeType (2.2f,
                                        juce::PathStrokeType::JointStyle::curved,
                                        juce::PathStrokeType::EndCapStyle::rounded));

    const float slotW = bossR * 3.4f;
    const float slotH = bossR * 0.85f;
    juce::Rectangle<float> slotRect (pivotX - slotW * 0.5f,
                                     bossY - slotH * 0.10f,
                                     slotW,
                                     slotH);
    g.setColour (juce::Colour (0xff8a7a4a));
    g.fillRoundedRectangle (slotRect, slotH * 0.5f);
    g.setColour (juce::Colour (0xff322a18));
    g.drawRoundedRectangle (slotRect, slotH * 0.5f, 0.8f);

    g.setColour (juce::Colour (0xff111111));
    g.fillEllipse (pivotX - bossR, bossY - bossR, bossR * 2.0f, bossR * 2.0f);
    g.setColour (juce::Colour (0xffb8a060));
    g.drawEllipse (pivotX - bossR, bossY - bossR, bossR * 2.0f, bossR * 2.0f, 1.3f);
    
    g.setColour (juce::Colour (0xffe8d690).withAlpha (0.7f));
    g.fillEllipse (pivotX - bossR * 0.35f,
                   bossY - bossR * 0.55f,
                   bossR * 0.5f,
                   bossR * 0.5f);

    juce::ignoreUnused (faceW);
}

void ABI1176Editor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    if (bgImage.isValid())
    {
        g.drawImage (bgImage, getLocalBounds().toFloat());
    }

    drawVUMeter (g);
}

void ABI1176Editor::resized()
{
    const int controlSize = 130;
    const int y = 52;
    const int startX = 110;
    const int spacing = 215;

    inputKnob .setBounds (startX + spacing * 0, y, controlSize, controlSize);
    driveKnob .setBounds (startX + spacing * 1, y, controlSize, controlSize);
    outputKnob.setBounds (startX + spacing * 2, y, controlSize, controlSize);
    mixKnob   .setBounds (startX + spacing * 3, y, controlSize, controlSize);

    const int toggleW = 41;
    const int toggleH = 22;
    const int labelH  = 14;
    const int labelGap = 2;

    const auto previousCentre = [] (int slotIndex)
    {
        return 365 + (34 + 22) * slotIndex + 34 / 2;
    };

    const int bottomToggleY = 195;
    const int bottomLabelY  = bottomToggleY + toggleH + labelGap;

    auto placeBottomToggle = [&] (juce::TextButton& btn, juce::Label& lbl, int slotIndex)
    {
        const int cx = previousCentre (slotIndex);
        btn.setBounds (cx - toggleW / 2, bottomToggleY, toggleW, toggleH);
        lbl.setBounds (cx - 60, bottomLabelY, 120, labelH);
    };

    placeBottomToggle (btnNuke,    lblNuke,    1);
    placeBottomToggle (btnMode,    lblMode,    2);
    placeBottomToggle (btnZeroLat, lblZeroLat, 3);
    placeBottomToggle (btnFat,     lblFat,     4);

    const int bypassX = 50;
    const int bypassY = 22;
    btnBypass.setBounds (bypassX, bypassY, toggleW, toggleH);
    lblBypass.setBounds (bypassX + toggleW / 2 - 60,
                         bypassY + toggleH + labelGap,
                         120,
                         labelH);
}