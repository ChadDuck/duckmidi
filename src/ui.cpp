#include "ui.h"
#include "piano_icon.h"
#include "toolbar_icons.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"
#include "text/en.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static float interface_scale = 1;
static SDL_Window *cursor_window;
static SDL_Cursor *icon_cursors[3], *system_cursors[ImGuiMouseCursor_COUNT];
static SDL_Cursor *applied_cursor;
static int cursor_size, gesture_cursor = -1;
static bool custom_cursor = true;
static void cache_cursors();

void ui_set_scale(float scale) {
    interface_scale = scale;

    if (cursor_window) {
        cache_cursors();
    }
}

float ui_scale() {
    return interface_scale;
}

UiPoint ui_display() {
    return {ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y};
}

static ImVec2 vec(UiPoint p) {
    return ImVec2(p.x, p.y);
}

static UiPoint point(ImVec2 p) {
    return {p.x, p.y};
}

bool ui_init(SDL_Window *window, SDL_Renderer *renderer, const char *settings) {
    interface_scale = 1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = settings;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {14, 12};
    s.FramePadding = {9, 6};
    s.ItemSpacing = {8, 8};
    s.WindowRounding = 9;
    s.ChildRounding = 7;
    s.FrameRounding = 5;
    s.PopupRounding = 7;
    s.ScrollbarSize = 12;
    s.WindowBorderSize = 1;
    s.Colors[ImGuiCol_WindowBg] = {0.055f, 0.067f, 0.09f, 1};
    s.Colors[ImGuiCol_ChildBg] = {0.07f, 0.085f, 0.11f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.12f, 0.15f, 0.19f, 1};
    s.Colors[ImGuiCol_Button] = {0.13f, 0.18f, 0.22f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.19f, 0.31f, 0.34f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.14f, 0.43f, 0.4f, 1};
    s.Colors[ImGuiCol_Header] = {0.15f, 0.38f, 0.35f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.18f, 0.33f, 0.34f, 1};
    s.Colors[ImGuiCol_CheckMark] = {0.4f, 0.88f, 0.69f, 1};
    s.Colors[ImGuiCol_SliderGrab] = {0.34f, 0.78f, 0.62f, 1};
    s.Colors[ImGuiCol_Text] = {0.88f, 0.92f, 0.95f, 1};
    io.Fonts->AddFontDefault();
    s.FontSizeBase = 15;
    bool ok = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer) &&
              ImGui_ImplSDLRenderer3_Init(renderer);

    if (ok) {
        cursor_window = window;
        custom_cursor = true;
        cache_cursors();
    }

    return ok;
}

void ui_shutdown() {
    SDL_SetCursor(nullptr);

    for (auto &cursor : icon_cursors) {
        SDL_DestroyCursor(cursor);
        cursor = nullptr;
    }

    for (auto &cursor : system_cursors) {
        SDL_DestroyCursor(cursor);
        cursor = nullptr;
    }

    cursor_window = nullptr;
    applied_cursor = nullptr;
    cursor_size = 0;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ui_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED && cursor_window) {
        cache_cursors();
    }

    SDL_Event scaled = *e;

    if (scaled.type == SDL_EVENT_MOUSE_MOTION) {
        scaled.motion.x /= interface_scale;
        scaled.motion.y /= interface_scale;
        scaled.motion.xrel /= interface_scale;
        scaled.motion.yrel /= interface_scale;
    }

    ImGui_ImplSDL3_ProcessEvent(&scaled);
}

void ui_frame() {
    gesture_cursor = -1;
    ImGui_ImplSDLRenderer3_NewFrame();
    auto &context = *ImGui::GetCurrentContext();
    int pending = context.InputEventsQueue.Size;
    ImGui_ImplSDL3_NewFrame();
    // Backend fallback mouse polling uses physical window coordinates.
    for (int i = pending; i < context.InputEventsQueue.Size; ++i) {
        auto &event = context.InputEventsQueue[i];

        if (event.Type == ImGuiInputEventType_MousePos) {
            event.MousePos.PosX /= interface_scale;
            event.MousePos.PosY /= interface_scale;
        }
    }

    auto &io = ImGui::GetIO();
    io.DisplaySize.x /= interface_scale;
    io.DisplaySize.y /= interface_scale;
    io.DisplayFramebufferScale.x *= interface_scale;
    io.DisplayFramebufferScale.y *= interface_scale;
    ImGui::NewFrame();
}

void ui_render(SDL_Renderer *r) {
    ImGui::Render();
    int requested = ImGui::GetMouseCursor();

    if (requested >= 0 && requested < ImGuiMouseCursor_COUNT) {
        SDL_Cursor *cursor = system_cursors[requested];

        if (gesture_cursor >= 0) {
            cursor = custom_cursor ? icon_cursors[gesture_cursor]
                                   : system_cursors[gesture_cursor == 1 ? ImGuiMouseCursor_ResizeAll
                                                                        : ImGuiMouseCursor_Hand];
        } else if (custom_cursor && requested == ImGuiMouseCursor_Arrow) {
            cursor = icon_cursors[0];
        }

        if (!cursor) {
            cursor = system_cursors[requested];
        }

        if (cursor && cursor != applied_cursor) {
            SDL_SetCursor(cursor);
            applied_cursor = cursor;
        }
    }

    float old_x, old_y;
    SDL_GetRenderScale(r, &old_x, &old_y);
    const auto &scale = ImGui::GetIO().DisplayFramebufferScale;
    SDL_SetRenderScale(r, scale.x, scale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r);
    SDL_SetRenderScale(r, old_x, old_y);
}

void ui_root(float y, float h) {
    ImGui::SetNextWindowPos({0, y});
    ImGui::SetNextWindowSize({ImGui::GetIO().DisplaySize.x, h});
    ImGui::Begin("##workspace", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
}

void ui_end() {
    ImGui::End();
}

bool ui_child(const char *id, float w, float h, bool scroll) {
    return ImGui::BeginChild(id, {w, h}, ImGuiChildFlags_Borders,
                             scroll ? ImGuiWindowFlags_HorizontalScrollbar
                                    : ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse);
}

void ui_compact_begin() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {6, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2, 4});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4, 4});
}

bool ui_mixer_child(const char *id, float w, float h, bool group) {
    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    background.w = group ? .35f : 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, group ? ImVec2(6, 6) : ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, group ? 5.0f : 0.0f);
    bool visible =
        ImGui::BeginChild(id, {w, h}, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return visible;
}

bool ui_mixer_header(const char *label, float width) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {.5f, .5f});
    bool clicked = ImGui::Button(label, {width, 24});
    ImGui::PopStyleVar();
    return clicked;
}

bool ui_mixer_pan(int *value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::SliderInt("##pan", value, 0, 127, "");
}

void ui_compact_end() {
    ImGui::PopStyleVar(3);
}

void ui_child_end() {
    ImGui::EndChild();
}

void ui_position(float x, float y) {
    ImGui::SetCursorScreenPos({x, y});
}

UiPoint ui_cursor() {
    return point(ImGui::GetCursorScreenPos());
}

UiPoint ui_available() {
    return point(ImGui::GetContentRegionAvail());
}

UiPoint ui_window() {
    return point(ImGui::GetWindowPos());
}

UiPoint ui_scroll_value() {
    return {ImGui::GetScrollX(), ImGui::GetScrollY()};
}

UiMouse ui_mouse() {
    auto &io = ImGui::GetIO();
    auto &context = *ImGui::GetCurrentContext();
    bool owned = context.CurrentWindow && context.HoveredWindow &&
                 context.CurrentWindow->RootWindow == context.HoveredWindow->RootWindow;
    return {point(io.MousePos),
            point(io.MouseDelta),
            owned ? io.MouseWheel : 0,
            io.MouseDown[0],
            owned && ImGui::IsMouseClicked(0),
            ImGui::IsMouseReleased(0),
            io.MouseDown[1],
            owned && ImGui::IsMouseClicked(1),
            owned && ImGui::IsMouseDoubleClicked(0),
            io.KeyShift,
            io.KeyCtrl,
            io.KeyAlt,
            owned && ImGui::IsMouseClicked(2),
            io.MouseDown[2],
            ImGui::IsMouseReleased(2)};
}

float ui_delta_time() {
    return ImMin(.05f, ImGui::GetIO().DeltaTime);
}

void ui_width(float width) {
    ImGui::SetNextItemWidth(width);
}

void ui_newline() {
    ImGui::NewLine();
}

void ui_group_begin() {
    ImGui::BeginGroup();
}

void ui_group_end() {
    ImGui::EndGroup();
}

void ui_same() {
    ImGui::SameLine();
}

float ui_item_spacing_x() {
    return ImGui::GetStyle().ItemSpacing.x;
}

float ui_item_spacing_y() {
    return ImGui::GetStyle().ItemSpacing.y;
}

void ui_space(float h) {
    ImGui::Dummy({0, h});
}

void ui_separator() {
    ImGui::Separator();
}

void ui_text(const char *t) {
    ImGui::TextUnformatted(t);
}

void ui_wrap(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
}

void ui_dim(const char *t) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.47f, .55f, .62f, 1));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

void ui_tip(const char *t) {
    /* Long tips wrap onto more lines instead of being cut off. */
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        float width = ImMax(160.f, ImMin(420.f, ImGui::GetMainViewport()->Size.x - 50));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(width);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void ui_info(const char *text) {
    auto pos = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::InvisibleButton(DM_TEXT_EVENT_INFORMATION, {24, h});
    auto *draw = ImGui::GetWindowDrawList();
    ImU32 color =
        ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw->AddCircle({pos.x + 12, pos.y + h * .5f}, 8, color, 16, 1.5f);
    /* The glyph is centered on the circle by its measured width at every UI size. */
    draw->AddText(
        {pos.x + 12 - ImGui::CalcTextSize("i").x * .5f, pos.y + (h - ImGui::GetFontSize()) * .5f},
        color, "i");

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImMin(380.f, ImGui::GetMainViewport()->Size.x - 50));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool ui_frame_toggle(const char *label, bool *value) {
    ui_accent(*value);
    bool clicked = ImGui::Button(label, {0, ImGui::GetFrameHeight()});
    ui_accent_end();

    if (clicked) {
        *value = !*value;
    }

    return clicked;
}

float ui_scrollbar_reserved() {
    return ImGui::GetCurrentWindow()->ScrollbarSizes.y;
}

void ui_mixer_wheel() {
    auto *window = ImGui::GetCurrentWindow();
    window->Flags |= ImGuiWindowFlags_NoScrollWithMouse;

    /* The strips scroll only sideways; the horizontal scrollbar uses space kept free for it. */
    if (window->Scroll.y != 0) {
        ImGui::SetScrollY(0);
    }

    auto &io = ImGui::GetIO();

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        (io.MouseWheel || io.MouseWheelH)) {
        ImGui::SetScrollX(ImClamp(window->Scroll.x - (io.MouseWheel + io.MouseWheelH) * 70, 0.f,
                                  window->ScrollMax.x));
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, window->ID);
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, window->ID);
    }
}

void ui_piano_icon(UiRect r) {
    auto *draw = ImGui::GetWindowDrawList();

    for (const auto &key : piano_rects) {
        ImVec2 start = {r.x + key.x * r.w / 70, r.y + key.y * r.h / 40};
        draw->AddRectFilled(start, {start.x + key.w * r.w / 70, start.y + key.h * r.h / 40},
                            key.color, key.radius * r.w / 70);
    }
}

bool ui_button(const char *s, float w, float h) {
    return ImGui::Button(s, {w, h});
}

static ImRect icon_bounds(UiIcon icon) {
    ImVec2 lo(100, 100), hi(-100, -100);

    for (const auto &shape : toolbar_shapes) {
        if (shape.icon != icon) {
            continue;
        }

        const float *v = shape.values;

        if (shape.kind == 2) {
            lo = ImMin(lo, {v[0], v[1]});
            hi = ImMax(hi, {v[0] + v[2], v[1] + v[3]});
        } else if (shape.kind == 3) {
            lo = ImMin(lo, {v[0] - v[2], v[1] - v[2]});
            hi = ImMax(hi, {v[0] + v[2], v[1] + v[2]});
        } else {
            for (int i = 0; i < shape.count; i += 2) {
                lo = ImMin(lo, {v[i], v[i + 1]});
                hi = ImMax(hi, {v[i], v[i + 1]});
            }
        }
    }

    return {lo, hi};
}

static void draw_icon(UiIcon icon, ImVec2 center, float size, ImU32 color) {
    auto *draw = ImGui::GetWindowDrawList();
    ImVec2 origin = icon_bounds(icon).GetCenter();
    float scale = size / 24, stroke = (icon == UI_PIANO ? .9f : 1.5f) * scale;

    for (const auto &shape : toolbar_shapes) {
        if (shape.icon != icon) {
            continue;
        }

        const float *v = shape.values;
        ImVec2 start(center.x + (v[0] - origin.x) * scale, center.y + (v[1] - origin.y) * scale);

        if (shape.kind == 2) {
            draw->AddRect(start, {start.x + v[2] * scale, start.y + v[3] * scale}, color,
                          v[4] * scale, 0, stroke);
        } else if (shape.kind == 3) {
            draw->AddCircle(start, v[2] * scale, color, 24, stroke);
        } else {
            ImVec2 points[256];
            int count = shape.count / 2;

            for (int i = 0; i < count; ++i) {
                points[i] = {center.x + (v[i * 2] - origin.x) * scale,
                             center.y + (v[i * 2 + 1] - origin.y) * scale};
            }

            draw->AddPolyline(points, count, color, shape.kind == 1 ? ImDrawFlags_Closed : 0,
                              stroke);
        }
    }
}

static float segment_distance(float x, float y, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
    float t = length > 0 ? ImClamp(((x - ax) * dx + (y - ay) * dy) / length, 0.f, 1.f) : 0;
    return std::hypot(x - ax - t * dx, y - ay - t * dy);
}

/* Even-odd test against a closed shape's outline. */
static bool inside_shape(const float *v, int points, float x, float y) {
    bool inside = false;

    for (int i = 0, j = points - 1; i < points; j = i++) {
        float xi = v[2 * i], yi = v[2 * i + 1], xj = v[2 * j], yj = v[2 * j + 1];

        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }

    return inside;
}

/* filled paints the inside of closed shapes in the outline color. */
static SDL_Cursor *make_icon_cursor(UiIcon icon, int size, float hx, float hy, bool filled) {
    SDL_Surface *surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);

    if (!surface) {
        return nullptr;
    }

    float scale = (size - 4.f) / 24;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float px = (x + .5f - 2) / scale, py = (y + .5f - 2) / scale, distance = 100;

            for (const auto &shape : toolbar_shapes) {
                if (shape.icon != icon) {
                    continue;
                }

                int points = shape.count / 2;

                if (filled && shape.kind == 1 && inside_shape(shape.values, points, px, py)) {
                    distance = 0;
                    break;
                }

                for (int i = 0; i < points - 1 + shape.kind; ++i) {
                    int next = (i + 1) % points;
                    distance =
                        ImMin(distance,
                              segment_distance(px, py, shape.values[2 * i], shape.values[2 * i + 1],
                                               shape.values[2 * next], shape.values[2 * next + 1]));
                }
            }

            distance *= scale;
            float white = ImClamp(.75f * scale + .5f - distance, 0.f, 1.f);
            float alpha = ImClamp(.75f * scale + 1.5f - distance, 0.f, 1.f);
            Uint8 shade = alpha > 0 ? (Uint8)(255 * white / alpha) : 0;
            auto *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
            row[x] = SDL_MapSurfaceRGBA(surface, shade, shade, shade, (Uint8)(255 * alpha));
        }
    }

    SDL_Cursor *cursor =
        SDL_CreateColorCursor(surface, (int)(2 + hx * scale), (int)(2 + hy * scale));
    SDL_DestroySurface(surface);
    return cursor;
}

static void cache_cursors() {
    const SDL_SystemCursor types[] = {
        SDL_SYSTEM_CURSOR_DEFAULT,     SDL_SYSTEM_CURSOR_TEXT,       SDL_SYSTEM_CURSOR_MOVE,
        SDL_SYSTEM_CURSOR_NS_RESIZE,   SDL_SYSTEM_CURSOR_EW_RESIZE,  SDL_SYSTEM_CURSOR_NESW_RESIZE,
        SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_POINTER,    SDL_SYSTEM_CURSOR_WAIT,
        SDL_SYSTEM_CURSOR_PROGRESS,    SDL_SYSTEM_CURSOR_NOT_ALLOWED};

    for (int i = 0; i < ImGuiMouseCursor_COUNT; ++i) {
        if (!system_cursors[i]) {
            system_cursors[i] = SDL_CreateSystemCursor(types[i]);
        }
    }

    int size = (int)std::round(32 * interface_scale * SDL_GetWindowPixelDensity(cursor_window));
    size = ImClamp(size, 24, 96);

    if (size == cursor_size) {
        return;
    }

    SDL_SetCursor(system_cursors[0]);
    applied_cursor = nullptr;

    for (auto &cursor : icon_cursors) {
        SDL_DestroyCursor(cursor);
        cursor = nullptr;
    }

    icon_cursors[0] = make_icon_cursor(UI_CURSOR, size, 5, 5, true);
    icon_cursors[1] = make_icon_cursor(UI_DRAG_HAND, size, 12, 12, false);
    icon_cursors[2] = make_icon_cursor(UI_POINT_HAND, size, 10, 2, false);
    cursor_size = size;
}

void ui_custom_cursor(bool enabled) {
    custom_cursor = enabled;
}

void ui_gesture_cursor(bool point) {
    gesture_cursor = point ? 2 : 1;
}

bool ui_editor_menu() {
    ImVec2 at = ImGui::GetCursorScreenPos();

    if (ImGui::InvisibleButton("##editor menu", {18, 28})) {
        ImGui::OpenPopup("##editor actions");
    }

    bool open = ImGui::IsPopupOpen("##editor actions");

    if (ImGui::IsItemHovered() || open) {
        ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + 18, at.y + 28},
                                                  ImGui::GetColorU32(ImGui::IsItemActive()
                                                                         ? ImGuiCol_ButtonActive
                                                                         : ImGuiCol_ButtonHovered),
                                                  3);
    }

    auto *draw = ImGui::GetWindowDrawList();
    ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);

    if (open) {
        draw->AddTriangleFilled({at.x + 4, at.y + 11}, {at.x + 14, at.y + 11},
                                {at.x + 9, at.y + 17}, color);
    } else {
        draw->AddTriangleFilled({at.x + 6, at.y + 8}, {at.x + 6, at.y + 20}, {at.x + 12, at.y + 14},
                                color);
    }

    if (open) {
        ImGui::SetNextWindowPos({at.x, at.y + 30}, ImGuiCond_Appearing);
    }

    return ImGui::BeginPopup("##editor actions");
}

bool ui_icon_button(const char *id, UiIcon icon, float width, float height) {
    if (!height) {
        height = ImGui::GetFrameHeight();
    }

    bool pressed = ImGui::Button(id, {width, height});
    ImVec2 center = ImGui::GetItemRectMin();
    center.x += width * .5f;
    center.y += height * .5f;
    draw_icon(icon, center, ImMin(20.f, height - 10), ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

static bool wheel_choice(bool selected, bool *chosen);

bool ui_select(const char *s, bool selected, float w, float h) {
    bool chosen;

    if (wheel_choice(selected, &chosen)) {
        return chosen;
    }

    bool pressed = ImGui::Selectable(s, selected, 0, {w, h});

    /* Dropdowns open scrolled to their current choice. */
    if (selected) {
        ImGui::SetItemDefaultFocus();
    }

    return pressed;
}

bool ui_check(const char *s, bool *v) {
    return ImGui::Checkbox(s, v);
}

bool ui_int(const char *s, int *v, int lo, int hi) {
    ImGui::SetNextItemWidth(125);
    bool changed = ImGui::InputInt(s, v, 1, 10);

    if (ImGui::IsItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            ++*v;
            changed = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            --*v;
            changed = true;
        }
    }

    if (changed) {
        *v = ImClamp(*v, lo, hi);
    }

    return changed;
}

bool ui_float(const char *s, float *v, float lo, float hi) {
    auto &style = ImGui::GetStyle();
    float content = ImGui::CalcTextSize("999.00").x + style.FramePadding.x * 2;
    ImGui::SetNextItemWidth(
        ImMax(125.0f, content + 2 * (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x)));
    bool changed = ImGui::InputFloat(s, v, 1, 10, "%.2f");

    if (ImGui::IsItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            *v += 1;
            changed = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            *v -= 1;
            changed = true;
        }
    }

    if (changed) {
        *v = ImClamp(*v, lo, hi);
    }

    return changed;
}

bool ui_input(const char *s, char *v, size_t n) {
    ImGui::SetNextItemWidth(240);
    return ImGui::InputText(s, v, n);
}

bool ui_appearing() {
    return ImGui::IsWindowAppearing();
}

bool ui_dialog_enter() {
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ui_popup_open() &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, ImGuiKeyOwner_Any) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None, ImGuiKeyOwner_Any));
}

bool ui_content_dialog(const char *title, bool *open, float width) {
    ImGui::SetNextWindowSize({width, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    return ImGui::Begin(title, open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_AlwaysAutoResize);
}

bool ui_mixer_value(int *value, float width) {
    char label[16];
    std::snprintf(label, sizeof(label), "%d", *value);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {.5f, .5f});
    bool clicked = ImGui::Button(label, {width, 24});
    ImGui::PopStyleVar();

    if (clicked) {
        ImGui::OpenPopup(DM_TEXT_EDIT_INITIAL_VOLUME);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("volume-draft"), *value);
    }

    bool changed = false;

    if (ImGui::BeginPopup(DM_TEXT_EDIT_INITIAL_VOLUME)) {
        auto *parent = ImGui::GetCurrentWindow()->ParentWindow;
        ImGuiID key = parent->GetID("volume-draft");
        int draft = parent->StateStorage.GetInt(key, *value);

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(120);
        bool submit = ImGui::InputInt("Volume", &draft, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
        parent->StateStorage.SetInt(key, draft);

        if (draft < 0 || draft > 127) {
            ImGui::TextUnformatted(DM_TEXT_ENTER_VALUE_FROM_0);
        }

        if (submit && draft >= 0 && draft <= 127) {
            changed = draft != *value;
            *value = draft;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return changed;
}

bool ui_multiline(const char *s, char *v, size_t n, float height) {
    return ImGui::InputTextMultiline(s, v, n, {ImGui::GetContentRegionAvail().x, height});
}

bool ui_knob(const char *label, double *value, double lo, double hi, double reset) {
    ImGui::BeginGroup();
    ImGui::PushID(label);
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##knob", ImVec2(60, 72),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool changed = false;
    ImGuiStorage *storage = ImGui::GetStateStorage();
    ImGuiID drag_id = ImGui::GetID("##raw-value");

    if (ImGui::IsItemActivated()) {
        storage->SetFloat(drag_id, (float)*value);
    }

    if (ImGui::IsItemActive() && !ImGui::IsItemActivated() && ImGui::IsMouseDown(0) &&
        ImGui::GetIO().MouseDelta.y) {
        double raw = storage->GetFloat(drag_id, (float)*value);
        double delta = -ImGui::GetIO().MouseDelta.y / 180;
        *value = ImClamp(std::strcmp(label, DM_TEXT_SPEED) == 0 ? raw * std::pow(hi / lo, delta)
                                                                : raw + delta * (hi - lo),
                         lo, hi);
        storage->SetFloat(drag_id, (float)*value);
        changed = true;
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(2)) {
        *value = reset;
        changed = true;
    }

    auto *draw = ImGui::GetWindowDrawList();
    ImVec2 center(at.x + 30, at.y + 23);
    ImU32 color = ImGui::GetColorU32(ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                                     : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                              : ImGuiCol_Button);
    draw->AddCircleFilled(center, 18, color);
    draw->AddCircle(center, 18, ImGui::GetColorU32(ImGuiCol_Border));
    double angle = 2.35619449 + (*value - lo) / (hi - lo) * 4.71238898;
    draw->AddLine(center, ImVec2(center.x + cos(angle) * 13, center.y + sin(angle) * 13),
                  ImGui::GetColorU32(ImGuiCol_Text), 2);
    char text[32];
    snprintf(text, sizeof(text), "%.2f", *value);
    draw->AddText(ImVec2(center.x - ImGui::CalcTextSize(text).x * .5f, at.y + 44),
                  ImGui::GetColorU32(ImGuiCol_Text), text);
    const char *caption = std::strcmp(label, DM_TEXT_PULSE_WIDTH) == 0 ? DM_TEXT_WIDTH : label;
    draw->AddText(ImVec2(center.x - ImGui::CalcTextSize(caption).x * .5f, at.y + 59),
                  ImGui::GetColorU32(ImGuiCol_Text), caption);
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}

bool ui_slider(const char *s, int *v, int lo, int hi, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::SliderInt(s, v, lo, hi);
}

bool ui_vslider(const char *s, int *v, float height) {
    return ImGui::VSliderInt(s, {24, height}, v, 0, 127, "");
}

/* Mouse wheel over a closed dropdown steps to the neighbouring choice. The first frame runs the
   dropdown's choices without drawing them to find the selected one; the next frame reports the
   neighbour as clicked, so every dropdown steps through its own selection code. */
static struct {
    ImGuiID id;
    int phase; /* 0 idle, 1 finding the selected choice, 2 choosing the neighbour */
    int direction, index, selected, target, frame;
    bool active; /* The current dropdown is being stepped rather than drawn. */
} wheel_step;

static void wheel_begin() {
    /* Choices drawn while stepping land in an off-screen window, not the visible layout. */
    ImGui::SetNextWindowPos({-100000, -100000});
    ImGui::Begin("##dropdown wheel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
}

static bool combo_wheel(ImGuiID id) {
    int frame = ImGui::GetFrameCount();

    if (wheel_step.phase && frame > wheel_step.frame + 1) {
        wheel_step.phase = 0;
    }

    if (wheel_step.phase == 2 && wheel_step.id == id) {
        wheel_step.active = true;
        wheel_begin();
        wheel_step.index = 0;
        return true;
    }

    auto &io = ImGui::GetIO();

    if (wheel_step.phase || !ImGui::IsItemHovered() || !io.MouseWheel) {
        return false;
    }

    ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, id);
    wheel_step = {};
    wheel_step.id = id;
    wheel_step.phase = 1;
    wheel_step.direction = io.MouseWheel > 0 ? -1 : 1;
    wheel_step.selected = -1;
    wheel_step.frame = frame;
    wheel_step.active = true;
    wheel_begin();
    return true;
}

static bool combo(const char *s, const char *p, bool bar) {
    ImGuiID id = ImGui::GetID(s);
    bool open;

    if (bar) {
        auto padding = ImGui::GetStyle().FramePadding;
        padding.y = (32 - ImGui::GetFontSize()) * .5f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
        open = ImGui::BeginCombo(s, p);
        ImGui::PopStyleVar();
    } else {
        open = ImGui::BeginCombo(s, p);
    }

    return open || combo_wheel(id);
}

bool ui_combo(const char *s, const char *p) {
    return combo(s, p, false);
}

bool ui_bar_combo(const char *s, const char *p) {
    return combo(s, p, true);
}

void ui_bar_begin() {
    auto padding = ImGui::GetStyle().FramePadding;
    padding.y = (32 - ImGui::GetFontSize()) * .5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
}

void ui_bar_end() {
    ImGui::PopStyleVar();
}

bool ui_panel_clicked() {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
           (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2));
}

bool ui_workspace_focused() {
    auto *window = ImGui::GetCurrentContext()->NavWindow;
    return window && std::strcmp(window->RootWindow->Name, "##workspace") == 0;
}

bool ui_background_clicked() {
    return ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) &&
           ImGui::GetCurrentContext()->HoveredId == 0;
}

bool ui_shift_context(const char *s) {
    if (ImGui::GetIO().KeyShift && ImGui::IsItemHovered() && ImGui::IsMouseReleased(1)) {
        ImGui::OpenPopup(s);
    }

    return ImGui::BeginPopup(s);
}

/* While a dropdown is stepped, its choices are counted instead of drawn. */
static bool wheel_choice(bool selected, bool *chosen) {
    if (!wheel_step.active) {
        return false;
    }

    int index = wheel_step.index++;

    if (wheel_step.phase == 1 && selected) {
        wheel_step.selected = index;
    }

    *chosen = wheel_step.phase == 2 && index == wheel_step.target;
    return true;
}

void ui_combo_end() {
    if (!wheel_step.active) {
        ImGui::EndCombo();
        return;
    }

    wheel_step.active = false;
    ImGui::End();

    if (wheel_step.phase == 2) {
        wheel_step.phase = 0;
        return;
    }

    /* With nothing selected, the wheel starts at the first or last choice. */
    int count = wheel_step.index;
    int target = wheel_step.selected < 0 ? (wheel_step.direction > 0 ? 0 : count - 1)
                                         : wheel_step.selected + wheel_step.direction;
    wheel_step.phase = target >= 0 && target < count && target != wheel_step.selected ? 2 : 0;
    wheel_step.target = target;
}

void ui_id(int id) {
    ImGui::PushID(id);
}

void ui_pop_id() {
    ImGui::PopID();
}

bool ui_menu_bar() {
    return ImGui::BeginMainMenuBar();
}

void ui_menu_bar_end() {
    ImGui::EndMainMenuBar();
}

bool ui_menu(const char *s) {
    return ImGui::BeginMenu(s);
}

void ui_menu_end() {
    ImGui::EndMenu();
}

bool ui_menu_item(const char *s, const char *key, bool enabled) {
    return ImGui::MenuItem(s, key, false, enabled);
}

bool ui_menu_check(const char *s, bool checked) {
    return ImGui::MenuItem(s, nullptr, checked);
}

void ui_pan_cursor() {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}

void ui_love_text(const char *text) {
    const char *heart = std::strstr(text, "\xf0\x9f\x92\x9b");

    if (!heart) {
        ImGui::TextUnformatted(text);
        return;
    }

    ImGui::TextUnformatted(text, heart);
    ImGui::SameLine(0, 0);
    ImVec2 at = ImGui::GetCursorScreenPos();
    float size = ImGui::GetFontSize();
    ImGui::Dummy({size, size});
    // A vector heart keeps the requested yellow emoji legible with the pixel font.
    ImVec2 points[48];

    for (int i = 0; i < 48; ++i) {
        float t = 6.28318530718f * i / 48, s = std::sin(t);
        float x = 16 * s * s * s;
        float y = 13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t);
        points[i] = {at.x + size * (.5f + x / 36), at.y + size * (.46f - y / 36)};
    }

    ImGui::GetWindowDrawList()->AddConcavePolyFilled(points, 48, IM_COL32(255, 216, 68, 255));
}

static int spectrum_clip(ImVec2 *polygon, int count, const ImVec2 *mask, int mask_count) {
    ImVec2 output[64];

    for (int edge = 0; edge < mask_count && count; ++edge) {
        ImVec2 a = mask[edge], b = mask[(edge + 1) % mask_count];
        auto side = [a, b](ImVec2 p) {
            return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        };
        int used = 0;
        ImVec2 previous = polygon[count - 1];
        float before = side(previous);

        for (int i = 0; i < count; ++i) {
            ImVec2 point = polygon[i];
            float after = side(point);

            if ((before >= 0) != (after >= 0)) {
                float t = before / (before - after);
                output[used++] = {previous.x + (point.x - previous.x) * t,
                                  previous.y + (point.y - previous.y) * t};
            }

            if (after >= 0) {
                output[used++] = point;
            }

            previous = point;
            before = after;
        }

        count = used;
        std::memcpy(polygon, output, (size_t)count * sizeof(ImVec2));
    }

    return count;
}

void ui_spectrum(const float *bands, int count, float height, uint32_t color) {
    auto *window = ImGui::GetCurrentWindow();
    auto *draw = window->DrawList;
    float border = window->WindowBorderSize;
    ImVec2 start = {window->Pos.x + border, window->Pos.y + border};
    ImVec2 end = {window->Pos.x + window->Size.x - border, window->Pos.y + window->Size.y - border};
    float radius = ImClamp(window->WindowRounding - border, 0.f,
                           ImMin(end.x - start.x, end.y - start.y) * .5f);
    ImVec2 mask[36];
    int mask_count = 0;
    const ImVec2 centers[] = {{end.x - radius, start.y + radius},
                              {end.x - radius, end.y - radius},
                              {start.x + radius, end.y - radius},
                              {start.x + radius, start.y + radius}};

    for (int corner = 0; corner < 4; ++corner) {
        for (int step = 0; step <= 8; ++step) {
            float angle = (corner - 1 + step / 8.f) * 1.57079632679f;
            mask[mask_count++] = {centers[corner].x + radius * std::cos(angle),
                                  centers[corner].y + radius * std::sin(angle)};
        }
    }

    draw->PushClipRect(start, end, false);
    float width = (end.x - start.x) / count;

    for (int i = 0; i < count; ++i) {
        float h = ImClamp(bands[i], 0.f, 1.f) * height;

        if (h < .1f) {
            continue;
        }

        float left = start.x + i * width, right = start.x + (i + 1) * width;

        if (i) {
            left += 1;
        }

        if (i + 1 < count) {
            right -= 1;
        }

        ImVec2 polygon[64] = {{left, end.y - h}, {right, end.y - h}, {right, end.y}, {left, end.y}};
        int vertices = spectrum_clip(polygon, 4, mask, mask_count);

        if (vertices < 3) {
            continue;
        }

        draw->PrimReserve((vertices - 2) * 3, vertices);
        unsigned base = draw->_VtxCurrentIdx;

        for (int v = 1; v + 1 < vertices; ++v) {
            draw->PrimWriteIdx((ImDrawIdx)base);
            draw->PrimWriteIdx((ImDrawIdx)(base + v));
            draw->PrimWriteIdx((ImDrawIdx)(base + v + 1));
        }

        for (int v = 0; v < vertices; ++v) {
            float blend = ImClamp((polygon[v].y - (end.y - h)) / h, 0.f, 1.f);
            unsigned alpha = (unsigned)(136 - 85 * blend);
            draw->PrimWriteVtx(polygon[v], draw->_Data->TexUvWhitePixel,
                               (color & 0x00ffffffu) | (alpha << 24));
        }
    }

    draw->PopClipRect();
}

bool ui_fixed_dialog(const char *s, bool *open, float w, float h) {
    ImGui::SetNextWindowSize({w, h}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    return ImGui::Begin(s, open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
}

bool ui_fitted_dialog(const char *s, bool *open, float w) {
    /* Fixed width; the height follows the contents every frame. */
    /* The height is only known after the window has measured its contents, so it is centered
       again for its first frames, then left where the user drags it. */
    ImGuiWindow *existing = ImGui::FindWindowByName(s);
    ImGuiStorage *storage = ImGui::GetStateStorage();
    ImGuiID frames = ImGui::GetID(s);

    if (!existing || !existing->WasActive) {
        storage->SetInt(frames, 0);
    }

    if (storage->GetInt(frames, 0) < 4) {
        storage->SetInt(frames, storage->GetInt(frames, 0) + 1);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                                {.5f, .5f});
    }

    ImGui::SetNextWindowSizeConstraints({w, 0}, {w, FLT_MAX});
    return ImGui::Begin(s, open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_AlwaysAutoResize);
}

void ui_center_row(float width) {
    float left = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(left + ImMax(0.0f, (ImGui::GetContentRegionAvail().x - width) * .5f));
}

void ui_center_text(const char *text) {
    ui_center_row(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
}

int ui_welcome_content() {
    ImVec2 origin = ImGui::GetCursorScreenPos(), area = ImGui::GetContentRegionAvail();
    float line = ImGui::GetTextLineHeight(), spacing = ImGui::GetStyle().ItemSpacing.y;
    float height = 4 * line + 2 * spacing + 40 + 42;
    float y = origin.y + ImMax(0.0f, (area.y - height) * .5f);
    auto centered = [&](const char *text, float top, bool dim) {
        float width = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorScreenPos({origin.x + (area.x - width) * .5f, top});

        if (dim) {
            ui_dim(text);
        } else {
            ui_text(text);
        }
    };
    centered(DM_TEXT_APP_NAME, y, false);
    centered(DM_TEXT_LITTLE_SPACE_NEXT, y + line + spacing, true);
    float buttons = y + 2 * line + spacing + 20;
    float gap = ImGui::GetStyle().ItemSpacing.x, width = (area.x - gap) * .5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {.5f, .5f});
    ImGui::SetCursorScreenPos({origin.x, buttons});
    int action = ImGui::Button(DM_TEXT_NEW_MIDI, {width, 42}) ? 1 : 0;
    ImGui::SetCursorScreenPos({origin.x + width + gap, buttons});

    if (ImGui::Button(DM_TEXT_OPEN_MIDI_PROJECT, {width, 42})) {
        action = 2;
    }

    ImGui::PopStyleVar();
    centered(DM_TEXT_DROP_MID_MUS, buttons + 42 + 20, true);
    centered(DM_TEXT_CHOOSE_SOUNDFONT_EDIT, buttons + 42 + 20 + line + spacing, true);
    return action;
}

void ui_confirmation_message(const char *text) {
    float height =
        ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetFrameHeight();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         ImMax(0.0f, (ImGui::GetContentRegionAvail().y - height) * .5f));
    ui_center_text(text);
    ImGui::Dummy({0, 0});
}

bool ui_dialog(const char *s, bool *open, float w, float h) {
    ImGui::SetNextWindowSize({w, h}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    return ImGui::Begin(s, open, ImGuiWindowFlags_NoCollapse);
}

bool ui_context(const char *s) {
    return ImGui::BeginPopupContextItem(s, ImGuiPopupFlags_MouseButtonRight);
}

bool ui_modal(const char *title, bool *open, float width, float height) {
    if (*open && !ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
    }

    if (!ImGui::IsPopupOpen(title)) {
        return false;
    }

    ImGui::SetNextWindowSize({width, height}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    return ImGui::BeginPopupModal(title, open, ImGuiWindowFlags_NoCollapse);
}

void ui_close_popup() {
    ImGui::CloseCurrentPopup();
}

void ui_popup_end() {
    ImGui::EndPopup();
}

bool ui_mouse_popup(const char *name, bool request) {
    if (request) {
        ImGui::OpenPopup(name);
    }

    ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
    return ImGui::BeginPopup(name);
}

bool ui_port_color(const char *id, uint32_t color, bool selected, float size) {
    bool clicked = ImGui::ColorButton(id, ImGui::ColorConvertU32ToFloat4(color),
                                      ImGuiColorEditFlags_NoTooltip, {size, size});

    if (selected) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            0xffffffff, 2, 0, 2);
    }

    return clicked;
}

bool ui_hover() {
    return ImGui::IsItemHovered();
}

bool ui_active() {
    return ImGui::IsItemActive();
}

bool ui_focused() {
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
}

bool ui_text_active() {
    return ImGui::GetIO().WantTextInput ||
           (ImGui::GetActiveID() != 0 && ImGui::GetInputTextState(ImGui::GetActiveID()) != nullptr);
}

bool ui_any_active() {
    return ImGui::IsAnyItemActive();
}

bool ui_item_changed() {
    return ImGui::IsItemDeactivatedAfterEdit();
}

void ui_disabled(bool value) {
    ImGui::BeginDisabled(value);
}

void ui_disabled_end() {
    ImGui::EndDisabled();
}

void ui_escape() {
    ImGui::ClearActiveID();
    ImGui::SetWindowFocus(nullptr);
}

int ui_topmost(const char *const *titles, int count) {
    auto &windows = ImGui::GetCurrentContext()->Windows;

    for (int w = windows.Size - 1; w >= 0; --w) {
        if (!windows[w]->Active) {
            continue;
        }

        for (int i = 0; i < count; ++i) {
            if (windows[w] == ImGui::FindWindowByName(titles[i])) {
                return i;
            }
        }
    }

    return -1;
}

bool ui_cancel_popup() {
    auto &g = *ImGui::GetCurrentContext();

    if (g.OpenPopupStack.Size == 0) {
        return false;
    }

    if (g.OpenPopupStack.back().Window &&
        (g.OpenPopupStack.back().Window->Flags & ImGuiWindowFlags_Modal)) {
        return false;
    }

    ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1, true);
    return true;
}

bool ui_popup_open() {
    return ImGui::GetCurrentContext()->OpenPopupStack.Size != 0;
}

void ui_raise_next() {
    ImGui::SetNextWindowFocus();
}

void ui_focus_next() {
    ImGui::SetKeyboardFocusHere();
}

void ui_canvas(const char *s, float w, float h) {
    ImGui::InvisibleButton(s, {w, h},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
}

void ui_scroll(float x, float y) {
    ImGui::SetScrollX(ImGui::GetScrollX() + x);
    ImGui::SetScrollY(ImGui::GetScrollY() + y);
}

void ui_scroll_to(float y) {
    ImGui::SetScrollY(y);
}

void ui_rect(UiRect r, uint32_t c, float rounding) {
    ImGui::GetWindowDrawList()->AddRectFilled({r.x, r.y}, {r.x + r.w, r.y + r.h}, c, rounding);
}

void ui_border(UiRect r, uint32_t c, float rounding, float width) {
    ImGui::GetWindowDrawList()->AddRect({r.x, r.y}, {r.x + r.w, r.y + r.h}, c, rounding, 0, width);
}

void ui_line(UiPoint a, UiPoint b, uint32_t c, float width) {
    ImGui::GetWindowDrawList()->AddLine(vec(a), vec(b), c, width);
}

void ui_label_fit(UiPoint at, float width, uint32_t color, const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), vec(at),
                              ImVec2(at.x + width, at.y + ImGui::GetFontSize()), at.x + width, text,
                              nullptr, nullptr);
    ImGui::PopStyleColor();
}

float ui_font_size() {
    return ImGui::GetFontSize();
}

float ui_text_width(const char *text) {
    return ImGui::CalcTextSize(text).x;
}

void ui_label(UiPoint p, uint32_t c, const char *s) {
    ImGui::GetWindowDrawList()->AddText(vec(p), c, s);
}

void ui_clip(UiRect r) {
    ImGui::GetWindowDrawList()->PushClipRect({r.x, r.y}, {r.x + r.w, r.y + r.h}, true);
}

void ui_unclip() {
    ImGui::GetWindowDrawList()->PopClipRect();
}

void ui_progress(float v, float w, float h, const char *s) {
    ImGui::ProgressBar(v, {w, h}, "");

    if (s && *s) {
        ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImVec2 text = ImGui::CalcTextSize(s);
        auto *draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(lo, hi, true);
        draw->AddText({lo.x + (hi.x - lo.x - text.x) * .5f, lo.y + (hi.y - lo.y - text.y) * .5f},
                      ImGui::GetColorU32(ImGuiCol_Text), s);
        draw->PopClipRect();
    }
}

void ui_align_frame() {
    ImGui::AlignTextToFramePadding();
    auto *window = ImGui::GetCurrentWindow();
    window->DC.CurrLineSize.y = ImMax(window->DC.CurrLineSize.y, 32.0f);
    window->DC.CurrLineTextBaseOffset =
        ImMax(window->DC.CurrLineTextBaseOffset, (32 - ImGui::GetFontSize()) * .5f);
}

float ui_button_row_width(const char *const *labels, int count) {
    float width = 0;

    for (int i = 0; i < count; ++i) {
        width += ImGui::CalcTextSize(labels[i]).x + ImGui::GetStyle().FramePadding.x * 2;

        if (i) {
            width += ImGui::GetStyle().ItemSpacing.x;
        }
    }

    return width;
}

void ui_channel_color(uint32_t color) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, c);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, c);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, c);
    ImGui::PushStyleColor(ImGuiCol_Border, c);
}

void ui_channel_color_end() {
    ImGui::PopStyleColor(4);
}

void ui_accent(bool on) {
    auto &s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          on ? s.Colors[ImGuiCol_HeaderActive] : s.Colors[ImGuiCol_Button]);
    ImGui::PushStyleColor(ImGuiCol_Text, s.Colors[ImGuiCol_Text]);
}

void ui_accent_end() {
    ImGui::PopStyleColor(2);
}

void ui_scrollbar(const char *id, UiRect r, bool vertical, float *value, float visible,
                  float total) {
    if (total <= visible + 1) {
        *value = 0;
        return;
    }

    ImS64 scroll = (ImS64)*value;
    ImGui::ScrollbarEx(ImRect(r.x, r.y, r.x + r.w, r.y + r.h), ImGui::GetID(id),
                       vertical ? ImGuiAxis_Y : ImGuiAxis_X, &scroll, (ImS64)visible, (ImS64)total);
    auto &io = ImGui::GetIO();

    if (ImGui::IsMouseHoveringRect({r.x, r.y}, {r.x + r.w, r.y + r.h}) &&
        ImGui::IsWindowHovered() && io.MouseWheel) {
        scroll = ImClamp(scroll - (ImS64)(io.MouseWheel * 70), (ImS64)0,
                         (ImS64)ImMax(0.f, total - visible));
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetID(id));
    }

    *value = (float)scroll;
}

void ui_theme(uint32_t accent) {
    auto &s = ImGui::GetStyle();
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(accent);

    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        if (i == ImGuiCol_Text || i == ImGuiCol_TextDisabled) {
            continue;
        }

        ImVec4 old = s.Colors[i];
        float gray = (old.x + old.y + old.z) / 3;
        s.Colors[i] = {gray, gray, gray, old.w};
    }

    s.Colors[ImGuiCol_WindowBg] = {.035f, .035f, .035f, 1};
    s.Colors[ImGuiCol_ChildBg] = {.055f, .055f, .055f, 1};
    s.Colors[ImGuiCol_FrameBg] = {.11f, .11f, .11f, 1};
    s.Colors[ImGuiCol_Button] = {.14f, .14f, .14f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {c.x * .34f, c.y * .34f, c.z * .34f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {c.x * .48f, c.y * .48f, c.z * .48f, 1};
    const int indices[] = {ImGuiCol_HeaderActive,     ImGuiCol_TitleBgActive,  ImGuiCol_SliderGrab,
                           ImGuiCol_SliderGrabActive, ImGuiCol_CheckMark,      ImGuiCol_NavCursor,
                           ImGuiCol_SeparatorHovered, ImGuiCol_SeparatorActive};

    for (int index : indices) {
        s.Colors[index] = {c.x * .55f, c.y * .55f, c.z * .55f, 1};
    }
}

bool ui_color(const char *label, uint32_t *color) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(*color);
    float rgb[] = {c.x, c.y, c.z};

    if (!ImGui::ColorEdit3(label, rgb, ImGuiColorEditFlags_NoInputs)) {
        return false;
    }

    *color = ImGui::ColorConvertFloat4ToU32({rgb[0], rgb[1], rgb[2], 1});
    return true;
}

bool ui_toggle(const char *label, bool *value) {
    ui_accent(*value);
    bool changed = ui_button(label, 0, 32);
    ui_accent_end();

    if (changed) {
        *value = !*value;
    }

    return changed;
}

void ui_resize_cursor(bool vertical) {
    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
}

void ui_triangle(UiPoint a, UiPoint b, UiPoint c, uint32_t color) {
    ImGui::GetWindowDrawList()->AddTriangleFilled(vec(a), vec(b), vec(c), color);
}

void ui_circle(UiPoint p, float radius, uint32_t color) {
    ImGui::GetWindowDrawList()->AddCircleFilled(vec(p), radius, color);
}

void ui_circle_outline(UiPoint p, float radius, uint32_t color) {
    ImGui::GetWindowDrawList()->AddCircle({p.x, p.y}, radius, color, 0, 1.5f);
}

/* The floating preview is drawn without the tooltip frame, so only the card shows. */
bool ui_drag_pattern(int pattern) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, 0);

    if (!ImGui::BeginDragDropSource()) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return false;
    }

    ImGui::SetDragDropPayload(DM_TEXT_DM_PATTERN, &pattern, sizeof(pattern));
    return true;
}

void ui_drag_pattern_end() {
    ImGui::EndDragDropSource();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

bool ui_invisible_button(const char *id, float w, float h) {
    return ImGui::InvisibleButton(id, {w, h});
}

void ui_dummy(float w, float h) {
    ImGui::Dummy({w, h});
}

int ui_drop_pattern() {
    int result = -1;

    if (ImGui::BeginDragDropTarget()) {
        if (const auto *p = ImGui::AcceptDragDropPayload(DM_TEXT_DM_PATTERN)) {
            result = *static_cast<const int *>(p->Data);
        }

        ImGui::EndDragDropTarget();
    }

    return result;
}

void ui_clamp_windows() {
    ImVec2 extent = ImGui::GetIO().DisplaySize;

    for (auto *w : ImGui::GetCurrentContext()->Windows) {
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) {
            continue;
        }

        if (!w->WasActive) {
            continue;
        }

        ImVec2 size(ImMin(w->Size.x, extent.x), ImMin(w->Size.y, extent.y));
        ImVec2 pos(ImClamp(w->Pos.x, 0.0f, ImMax(0.0f, extent.x - size.x)),
                   ImClamp(w->Pos.y, 0.0f, ImMax(0.0f, extent.y - size.y)));
        ImGui::SetWindowSize(w, size);
        ImGui::SetWindowPos(w, pos);
    }
}

void ui_keyboard_navigation(bool enabled) {
    auto &io = ImGui::GetIO();

    if (enabled) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    }
}

bool ui_item_middle() {
    return ImGui::IsItemHovered() && ImGui::IsMouseClicked(2);
}

void ui_placement_tip(const char *text) {
    auto &io = ImGui::GetIO();
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 at(ImClamp(io.MousePos.x, 4.0f, ImMax(4.0f, io.DisplaySize.x - size.x - 20)),
              io.MousePos.y + 24);
    auto *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(at, {at.x + size.x + 16, at.y + size.y + 10}, 0xb8202020u, 5);
    draw->AddRect(at, {at.x + size.x + 16, at.y + size.y + 10}, 0x887f7f7fu, 5);
    draw->AddText({at.x + 8, at.y + 5}, 0xffebe4d9u, text);
}

bool ui_splitter(float *width, float min, float max, float height) {
    ImGui::InvisibleButton("##panel divider", {8, height});
    bool active = ImGui::IsItemActive();

    if (active || ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    ImVec2 at = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddLine({at.x + 4, at.y}, {at.x + 4, at.y + height}, 0xff3a3a3au);

    if (active) {
        *width = ImClamp(*width + ImGui::GetIO().MouseDelta.x, min, max);
    }

    return active;
}

UiRect ui_window_rect() {
    auto *w = ImGui::GetCurrentWindow();
    return {w->Pos.x, w->Pos.y, w->Size.x, w->Size.y};
}

void ui_drop_overlay(UiRect rect, const char *text) {
    auto *draw = ImGui::GetForegroundDrawList();
    ImVec2 start(rect.x, rect.y), end(rect.x + rect.w, rect.y + rect.h);
    draw->AddRectFilled(start, end, 0xd0101010u, 8);
    draw->AddRect({start.x + 10, start.y + 10}, {end.x - 10, end.y - 10}, 0xff56eeffu, 8, 0, 2);
    ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText({rect.x + (rect.w - size.x) * .5f, rect.y + (rect.h - size.y) * .5f}, 0xffebe4d9u,
                  text);
}
