// ColorPicker.xapp — HSV color picker with hex/RGB clipboard copy and 8-color history
// Built with xkit SDK; communicates with com.freebsdx.Clipboard via XBus.

#include <xkit/application.h>
#include <xkit/widget.h>
#include <xkit/xbus_client.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <array>

using namespace fbx::xkit;
using namespace fbx::xrender;

static XBusClient* g_xbus = nullptr;

// ── HSV→RGB ──────────────────────────────────────────────────────────────────
static void hsv_to_rgb(float h, float s, float v,
                       float& r, float& g, float& b) noexcept {
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    if      (h < 60)  { r = c+m; g = x+m; b =   m; }
    else if (h < 120) { r = x+m; g = c+m; b =   m; }
    else if (h < 180) { r =   m; g = c+m; b = x+m; }
    else if (h < 240) { r =   m; g = x+m; b = c+m; }
    else if (h < 300) { r = x+m; g =   m; b = c+m; }
    else              { r = c+m; g =   m; b = x+m; }
}

// ── Format helpers ────────────────────────────────────────────────────────────
static std::string toHex(float r, float g, float b) noexcept {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
        static_cast<int>(r * 255.0f + 0.5f),
        static_cast<int>(g * 255.0f + 0.5f),
        static_cast<int>(b * 255.0f + 0.5f));
    return buf;
}

static std::string toRgb(float r, float g, float b) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)",
        static_cast<int>(r * 255.0f + 0.5f),
        static_cast<int>(g * 255.0f + 0.5f),
        static_cast<int>(b * 255.0f + 0.5f));
    return buf;
}

static void writeClipboard(const std::string& text) noexcept {
    if (!g_xbus) return;
    g_xbus->Call("com.freebsdx.Clipboard", 0x01,
                 {reinterpret_cast<const uint8_t*>(text.c_str()), text.size() + 1},
                 nullptr);
}

int main(int /*argc*/, char* /*argv*/[]) {
    // ── Shared state ──────────────────────────────────────────────────────────
    auto cp_h      = std::make_shared<float>(200.0f);
    auto cp_s      = std::make_shared<float>(0.70f);
    auto cp_v      = std::make_shared<float>(0.85f);
    auto cp_recent = std::make_shared<std::vector<std::array<float, 3>>>();
    auto update_cp = std::make_shared<std::function<void()>>();

    // ── Swatch & labels ───────────────────────────────────────────────────────
    auto cp_swatch = std::make_shared<FlexRow>();
    cp_swatch->setFixedSize(286, 64);
    cp_swatch->setBackgroundColor(Color{0.2f, 0.67f, 0.85f, 1.0f});
    cp_swatch->setMainAxisAlignment(MainAxisAlignment::Center);
    cp_swatch->setCrossAxisAlignment(CrossAxisAlignment::Center);

    auto cp_hex_lbl = std::make_shared<TextWidget>("#33AADA", 18.0f);
    cp_hex_lbl->setFontWeight(600);
    cp_hex_lbl->setColor(Color{1.0f, 1.0f, 1.0f, 1.0f});

    auto cp_rgb_lbl = std::make_shared<TextWidget>("rgb(51, 170, 218)", 11.0f);
    cp_rgb_lbl->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});

    // ── Slider labels ─────────────────────────────────────────────────────────
    auto cp_h_lbl = std::make_shared<TextWidget>("H: 200°", 11.0f);
    cp_h_lbl->setFixedSize(60.0f, 0.0f);
    cp_h_lbl->setColor(Color{0.75f, 0.75f, 0.80f, 1.0f});

    auto cp_s_lbl = std::make_shared<TextWidget>("S: 70%", 11.0f);
    cp_s_lbl->setFixedSize(60.0f, 0.0f);
    cp_s_lbl->setColor(Color{0.75f, 0.75f, 0.80f, 1.0f});

    auto cp_v_lbl = std::make_shared<TextWidget>("V: 85%", 11.0f);
    cp_v_lbl->setFixedSize(60.0f, 0.0f);
    cp_v_lbl->setColor(Color{0.75f, 0.75f, 0.80f, 1.0f});

    // ── Sliders ───────────────────────────────────────────────────────────────
    auto cp_h_slider = std::make_shared<SliderWidget>(
        *cp_h, 0.0f, 360.0f,
        [cp_h, update_cp](float v) {
            *cp_h = v;
            if (*update_cp) (*update_cp)();
        });

    auto cp_s_slider = std::make_shared<SliderWidget>(
        *cp_s, 0.0f, 1.0f,
        [cp_s, update_cp](float v) {
            *cp_s = v;
            if (*update_cp) (*update_cp)();
        });

    auto cp_v_slider = std::make_shared<SliderWidget>(
        *cp_v, 0.0f, 1.0f,
        [cp_v, update_cp](float v) {
            *cp_v = v;
            if (*update_cp) (*update_cp)();
        });

    cp_h_slider->setFixedSize(200.0f, 0.0f);
    cp_s_slider->setFixedSize(200.0f, 0.0f);
    cp_v_slider->setFixedSize(200.0f, 0.0f);

    // ── Recent color swatches row ─────────────────────────────────────────────
    auto cp_recent_row = std::make_shared<FlexRow>();
    cp_recent_row->setGap(5.0f);
    cp_recent_row->setFixedSize(0.0f, 28.0f);

    // ── Copy buttons (callbacks set in on_init via shared_ptr<function>) ──────
    auto on_copy_hex = std::make_shared<std::function<void()>>();
    auto on_copy_rgb = std::make_shared<std::function<void()>>();

    auto cp_copy_hex_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Copy Hex", 12.0f),
        [on_copy_hex]() { if (*on_copy_hex) (*on_copy_hex)(); });
    cp_copy_hex_btn->setPadding(8.0f);
    cp_copy_hex_btn->setBackgroundColor(Color{0.20f, 0.22f, 0.32f, 1.0f});

    auto cp_copy_rgb_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Copy RGB", 12.0f),
        [on_copy_rgb]() { if (*on_copy_rgb) (*on_copy_rgb)(); });
    cp_copy_rgb_btn->setPadding(8.0f);
    cp_copy_rgb_btn->setBackgroundColor(Color{0.20f, 0.22f, 0.32f, 1.0f});

    // ── Helper: build a single slider row ────────────────────────────────────
    auto mk_slider_row = [](std::shared_ptr<TextWidget> lbl,
                            std::shared_ptr<SliderWidget> slider) {
        auto row = std::make_shared<FlexRow>();
        row->setGap(8.0f);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        row->addChild(lbl);
        row->addChild(slider);
        return row;
    };

    // ── Swatch labels column ──────────────────────────────────────────────────
    auto swatch_label_col = std::make_shared<FlexColumn>();
    swatch_label_col->setMainAxisAlignment(MainAxisAlignment::Center);
    swatch_label_col->setGap(4.0f);
    swatch_label_col->addChild(cp_hex_lbl);
    swatch_label_col->addChild(cp_rgb_lbl);
    cp_swatch->addChild(swatch_label_col);

    // ── Button row ────────────────────────────────────────────────────────────
    auto btn_row = std::make_shared<FlexRow>();
    btn_row->setGap(10.0f);
    btn_row->setMainAxisAlignment(MainAxisAlignment::Center);
    btn_row->addChild(cp_copy_hex_btn);
    btn_row->addChild(cp_copy_rgb_btn);

    // ── Recent label ─────────────────────────────────────────────────────────
    auto recent_lbl = std::make_shared<TextWidget>("Recent", 11.0f);
    recent_lbl->setColor(Color{0.60f, 0.62f, 0.70f, 1.0f});

    // ── Main panel ────────────────────────────────────────────────────────────
    auto panel = std::make_shared<FlexColumn>();
    panel->setPadding(17.0f);
    panel->setGap(14.0f);
    panel->addChild(cp_swatch);
    panel->addChild(mk_slider_row(cp_h_lbl, cp_h_slider));
    panel->addChild(mk_slider_row(cp_s_lbl, cp_s_slider));
    panel->addChild(mk_slider_row(cp_v_lbl, cp_v_slider));
    panel->addChild(btn_row);
    panel->addChild(recent_lbl);
    panel->addChild(cp_recent_row);

    auto root = std::make_shared<ContainerWidget>(panel);
    root->setBackgroundColor(Color{0.12f, 0.13f, 0.19f, 1.0f});

    return XApplication::Run(root, "/tmp/freebsdx.sock", false, false,
        [=](XBusClient& xbus) {
            g_xbus = &xbus;
            XApplication::setTitle("ColorPicker");

            // ── Rebuild recent swatches ───────────────────────────────────────
            auto rebuild_recent = [=]() {
                cp_recent_row->clearChildren();
                for (const auto& col : *cp_recent) {
                    auto sw_inner = std::make_shared<FlexRow>();
                    sw_inner->setFixedSize(24.0f, 24.0f);
                    sw_inner->setBackgroundColor(Color{col[0], col[1], col[2], 1.0f});
                    auto sw_col = col;
                    auto sw = std::make_shared<ButtonWidget>(sw_inner,
                        [sw_col, cp_h, cp_s, cp_v, update_cp,
                         cp_h_slider, cp_s_slider, cp_v_slider]() {
                            float r = sw_col[0], g = sw_col[1], b = sw_col[2];
                            float mx = std::fmax(r, std::fmax(g, b));
                            float mn = std::fmin(r, std::fmin(g, b));
                            float delta = mx - mn;
                            float h = 0.0f, s = 0.0f, v = mx;
                            if (delta > 1e-6f) {
                                s = delta / mx;
                                if (mx == r)      h = 60.0f * std::fmod((g - b) / delta, 6.0f);
                                else if (mx == g) h = 60.0f * ((b - r) / delta + 2.0f);
                                else              h = 60.0f * ((r - g) / delta + 4.0f);
                                if (h < 0.0f) h += 360.0f;
                            }
                            *cp_h = h; *cp_s = s; *cp_v = v;
                            cp_h_slider->setValue(h);
                            cp_s_slider->setValue(s);
                            cp_v_slider->setValue(v);
                            if (*update_cp) (*update_cp)();
                        });
                    cp_recent_row->addChild(sw);
                }
            };

            // ── update_cp: recompute display from current HSV ─────────────────
            *update_cp = [=]() {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                hsv_to_rgb(*cp_h, *cp_s, *cp_v, r, g, b);

                cp_swatch->setBackgroundColor(Color{r, g, b, 1.0f});
                cp_hex_lbl->setText(toHex(r, g, b));
                cp_rgb_lbl->setText(toRgb(r, g, b));

                char hbuf[16], sbuf[12], vbuf[12];
                std::snprintf(hbuf, sizeof(hbuf), "H: %.0f°", *cp_h);
                std::snprintf(sbuf, sizeof(sbuf), "S: %.0f%%", *cp_s * 100.0f);
                std::snprintf(vbuf, sizeof(vbuf), "V: %.0f%%", *cp_v * 100.0f);
                cp_h_lbl->setText(hbuf);
                cp_s_lbl->setText(sbuf);
                cp_v_lbl->setText(vbuf);
            };

            // Initial render
            (*update_cp)();

            // ── Copy Hex ─────────────────────────────────────────────────────
            *on_copy_hex = [=]() {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                hsv_to_rgb(*cp_h, *cp_s, *cp_v, r, g, b);
                writeClipboard(toHex(r, g, b));

                std::array<float, 3> entry{r, g, b};
                cp_recent->erase(
                    std::remove_if(cp_recent->begin(), cp_recent->end(),
                        [&entry](const std::array<float, 3>& e) {
                            return std::fabs(e[0] - entry[0]) < 0.005f &&
                                   std::fabs(e[1] - entry[1]) < 0.005f &&
                                   std::fabs(e[2] - entry[2]) < 0.005f;
                        }),
                    cp_recent->end());
                cp_recent->insert(cp_recent->begin(), entry);
                if (cp_recent->size() > 8) cp_recent->resize(8);
                rebuild_recent();
            };

            // ── Copy RGB ─────────────────────────────────────────────────────
            *on_copy_rgb = [=]() {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                hsv_to_rgb(*cp_h, *cp_s, *cp_v, r, g, b);
                writeClipboard(toRgb(r, g, b));

                std::array<float, 3> entry{r, g, b};
                cp_recent->erase(
                    std::remove_if(cp_recent->begin(), cp_recent->end(),
                        [&entry](const std::array<float, 3>& e) {
                            return std::fabs(e[0] - entry[0]) < 0.005f &&
                                   std::fabs(e[1] - entry[1]) < 0.005f &&
                                   std::fabs(e[2] - entry[2]) < 0.005f;
                        }),
                    cp_recent->end());
                cp_recent->insert(cp_recent->begin(), entry);
                if (cp_recent->size() > 8) cp_recent->resize(8);
                rebuild_recent();
            };
        });
}
