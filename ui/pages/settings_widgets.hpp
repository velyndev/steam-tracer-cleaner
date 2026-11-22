#pragma once

// Small building blocks shared by the Settings and Exclusions pages. The page layout itself (body,
// cards, gaps) comes from ui/common.hpp.

#include <facet/facet.h>

#include <filesystem>

namespace stc::ui::pages::settings_widgets {

// The widest a text control in a settings row grows (units): the row fills the card, but a field
// stretched across a wide card ends far from its label.
inline constexpr float kRowFieldMaxWidth = 480.0F;

// How the settings search matches: words of the query anywhere in a label, description or
// keyword list, ignoring case. The page uses the same rule to decide which cards have hits.
inline constexpr facet::FilterSettings kFilter{};

// Label column of the settings rows on both pages (units, with its gap): wide enough for the
// longest label ("Run Quick Clean when I sign in") at every scale, so no label is cut.
inline constexpr float kLabelWidth = 240.0F;

// The control slot of a settings row pushes its width as the item width, and Facet widgets
// resolve a width of 0 to the pushed item width before their own natural width (a Button "fits
// its label" only while no item width is pushed). Rows whose control is a button, a pill or a
// line of text and a button call begin_row_content() after facet::BeginSettingsRow and
// end_row_content() before facet::EndSettingsRow: in between, widgets take their natural width
// and slot_width() is the slot's width in pixels. Not nestable.
void begin_row_content();
void end_row_content();
float slot_width();

// The width (units) for a control that should be `wanted` units wide but never wider than the
// row's slot (or the space left on the line outside a row).
float fit_width(float wanted);

// Moves the cursor down so a line of Body text that follows sits centered on a control of the
// inherited size on the same line (labels in settings rows line up with the control's text).
void align_text_to_control();
// The same for an item `height_px` tall (a small pill, a small button).
void align_to_control(float height_px);

// Several items on one line of a row's control slot, each centered on the control height of the
// line (so text, a small pill and a button share one center line, as the row's label does).
// Call next() before each item with that item's height in pixels (0: one control height).
class InlineLine {
public:
    InlineLine();
    void next(float item_height_px = 0.0F, float gap_px = -1.0F);
    void next_text() { next(text_height()); }
    static float text_height();

private:
    float top_;
    bool first_ = true;
};

// A folder path as a value: the path cut in the middle to fit (the full path as a tooltip and
// on keyboard focus), a copy button when `copy`, and a "Show in Explorer" button. Fills the
// row's slot (inside begin_row_content) or the available width. `id` keys the buttons.
void path_value(const char* id, const std::filesystem::path& path, bool copy = true);

}  // namespace stc::ui::pages::settings_widgets
