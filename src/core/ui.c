#include "core/ui.h"
#include "core/math.h"

#define UI_HOVER_SPEED 12.0f

bool UiBegin(UiContext* ui, int32_t width, int32_t height,
    float mouseX, float mouseY, bool mouseDown, bool mousePressed,
    float deltaSeconds)
{
    if (ui == NULL || width <= 0 || height <= 0)
    {
        return false;
    }

    // Масштаб ограничен обеими сторонами окна: меню больше не вылезает
    // за края в узких или почти квадратных окнах.
    float widthScale = (float)width / 640.0f;
    float heightScale = (float)height / 440.0f;
    float responsiveScale = widthScale < heightScale
        ? widthScale : heightScale;
    ui->scale = ScalarClamp(responsiveScale, 0.65f, 4.0f);

    int32_t pixelSize = (int32_t)(15.0f * ui->scale + 0.5f);
    if (pixelSize != ui->bakedPixelSize)
    {
        if (UiFontBake(&ui->font, pixelSize))
        {
            ui->bakedPixelSize = pixelSize;
            ui->fontDirty = true;
        }
        else if (ui->bakedPixelSize == 0)
        {
            return false;
        }
    }

    ui->mouseX = mouseX;
    ui->mouseY = mouseY;
    ui->mouseDown = mouseDown;
    ui->mousePressed = mousePressed;
    ui->deltaSeconds = deltaSeconds;
    ui->quadCount = 0;
    if (!mouseDown)
    {
        ui->activeId = 0;
    }
    return true;
}

void UiRelease(UiContext* ui)
{
    if (ui == NULL)
    {
        return;
    }

    UiFontRelease(&ui->font);
    ui->bakedPixelSize = 0;
    ui->fontDirty = false;
    ui->activeId = 0;
    ui->animationCount = 0;
    ui->quadCount = 0;
}

static uint32_t LerpColor(uint32_t from, uint32_t to, float t)
{
    t = ScalarClamp(t, 0.0f, 1.0f);
    uint32_t result = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8)
    {
        float a = (float)((from >> shift) & 255u);
        float b = (float)((to >> shift) & 255u);
        uint32_t channel = (uint32_t)(a + (b - a) * t + 0.5f);
        result |= (channel & 255u) << shift;
    }
    return result;
}

static RendererUiQuad* PushQuad(UiContext* ui)
{
    if (ui->quadCount == UI_MAX_DRAW_QUADS)
    {
        return NULL;
    }
    RendererUiQuad* quad = &ui->quads[ui->quadCount++];
    quad->uv[0] = quad->uv[1] = quad->uv[2] = quad->uv[3] = 0.0f;
    quad->cornerRadius = 0.0f;
    quad->flags = 0;
    quad->reserved = 0;
    return quad;
}

void UiRect(UiContext* ui, float x, float y, float width, float height,
    float cornerRadius, uint32_t color)
{
    RendererUiQuad* quad = PushQuad(ui);
    if (quad == NULL)
    {
        return;
    }
    quad->rect[0] = x;
    quad->rect[1] = y;
    quad->rect[2] = x + width;
    quad->rect[3] = y + height;
    quad->cornerRadius = cornerRadius;
    quad->colorRGBA = color;
}

void UiPanel(UiContext* ui, float x, float y, float width, float height)
{
    float shadow = UiScaled(ui, 7.0f);
    UiRect(ui, x - shadow, y - shadow + UiScaled(ui, 3.0f),
        width + shadow * 2.0f, height + shadow * 2.0f,
        UiScaled(ui, 20.0f), UiColor(0, 0, 0, 90));
    UiRect(ui, x, y, width, height,
        UiScaled(ui, 14.0f), UI_COLOR_PANEL);
}

void UiText(UiContext* ui, float x, float lineTopY, uint32_t color,
    const wchar_t* text)
{
    float penX = x;
    float baseline = lineTopY + ui->font.ascent;
    for (const wchar_t* character = text; *character != L'\0'; ++character)
    {
        const UiGlyph* glyph = UiFontFindGlyph(&ui->font, (uint16_t)*character);
        if (glyph == NULL)
        {
            continue;
        }
        if (glyph->width != 0 && glyph->height != 0)
        {
            RendererUiQuad* quad = PushQuad(ui);
            if (quad == NULL)
            {
                return;
            }
            float glyphX = penX + (float)glyph->offsetX;
            float glyphY = baseline - (float)glyph->offsetY;
            quad->rect[0] = glyphX;
            quad->rect[1] = glyphY;
            quad->rect[2] = glyphX + (float)glyph->width;
            quad->rect[3] = glyphY + (float)glyph->height;
            quad->uv[0] = glyph->u0;
            quad->uv[1] = glyph->v0;
            quad->uv[2] = glyph->u1;
            quad->uv[3] = glyph->v1;
            quad->colorRGBA = color;
            quad->flags = RENDERER_UI_QUAD_TEXT;
        }
        penX += glyph->advance;
    }
}

void UiTextCentered(UiContext* ui, float centerX, float lineTopY,
    uint32_t color, const wchar_t* text)
{
    float width = UiFontMeasure(&ui->font, text);
    UiText(ui, centerX - width * 0.5f, lineTopY, color, text);
}

float UiTextWidth(const UiContext* ui, const wchar_t* text)
{
    return UiFontMeasure(&ui->font, text);
}

float UiLabelValueRow(UiContext* ui, float x, float width, float y,
    const wchar_t* label, const wchar_t* value, float bottomGap)
{
    UiText(ui, x, y, UI_COLOR_TEXT_DIM, label);
    float valueX = x + width - UiTextWidth(ui, value);
    UiText(ui, valueX, y, UI_COLOR_ACCENT, value);
    return y + ui->font.lineHeight + bottomGap;
}

float UiAnimate(UiContext* ui, uint32_t id, bool towardOne)
{
    UiAnimation* animation = NULL;
    for (uint32_t i = 0; i < ui->animationCount; ++i)
    {
        if (ui->animations[i].id == id)
        {
            animation = &ui->animations[i];
            break;
        }
    }
    if (animation == NULL)
    {
        if (ui->animationCount == UI_MAX_ANIMATIONS)
        {
            return towardOne ? 1.0f : 0.0f;
        }
        animation = &ui->animations[ui->animationCount++];
        animation->id = id;
        animation->value = towardOne ? 1.0f : 0.0f;
        return animation->value;
    }

    float target = towardOne ? 1.0f : 0.0f;
    float step = ScalarClamp(ui->deltaSeconds * UI_HOVER_SPEED, 0.0f, 1.0f);
    animation->value += (target - animation->value) * step;
    animation->value = ScalarClamp(animation->value, 0.0f, 1.0f);
    return animation->value;
}

static bool MouseInside(const UiContext* ui, float x, float y,
    float width, float height)
{
    return ui->mouseX >= x && ui->mouseX < x + width
        && ui->mouseY >= y && ui->mouseY < y + height;
}

bool UiButton(UiContext* ui, uint32_t id, float x, float y,
    float width, float height, const wchar_t* label)
{
    bool hovered = ui->activeId == 0 && MouseInside(ui, x, y, width, height);
    float hover = UiAnimate(ui, id, hovered);

    uint32_t background = LerpColor(UI_COLOR_BUTTON, UI_COLOR_BUTTON_HOT, hover);
    if (hovered && ui->mouseDown)
    {
        background = UI_COLOR_BUTTON_HELD;
    }

    UiRect(ui, x, y, width, height, UiScaled(ui, 8.0f), background);
    float textTop = y + (height - ui->font.lineHeight) * 0.5f;
    UiTextCentered(ui, x + width * 0.5f, textTop, UI_COLOR_TEXT, label);

    return hovered && ui->mousePressed;
}

bool UiSliderInt(UiContext* ui, uint32_t id, float x, float y,
    float width, int32_t minimum, int32_t maximum, int32_t* value)
{
    float height = UiScaled(ui, 20.0f);
    float trackHeight = UiScaled(ui, 5.0f);
    float knobRadius = UiScaled(ui, 8.0f);

    bool hovered = ui->activeId == 0
        && MouseInside(ui, x - knobRadius, y, width + knobRadius * 2.0f, height);
    if (hovered && ui->mousePressed)
    {
        ui->activeId = id;
    }
    bool active = ui->activeId == id;

    bool changed = false;
    if (active && ui->mouseDown && maximum > minimum)
    {
        float t = ScalarClamp((ui->mouseX - x) / width, 0.0f, 1.0f);
        int32_t next = minimum
            + (int32_t)(t * (float)(maximum - minimum) + 0.5f);
        if (next != *value)
        {
            *value = next;
            changed = true;
        }
    }

    float t = maximum > minimum
        ? ((float)(*value - minimum) / (float)(maximum - minimum)) : 0.0f;
    t = ScalarClamp(t, 0.0f, 1.0f);

    float trackY = y + (height - trackHeight) * 0.5f;
    float knobX = x + width * t;

    UiRect(ui, x, trackY, width, trackHeight,
        trackHeight * 0.5f, UI_COLOR_TRACK);
    if (t > 0.0f)
    {
        UiRect(ui, x, trackY, width * t, trackHeight,
            trackHeight * 0.5f, UI_COLOR_ACCENT);
    }

    float hover = UiAnimate(ui, id, hovered || active);
    float radius = knobRadius * (1.0f + 0.18f * hover);
    UiRect(ui, knobX - radius, y + height * 0.5f - radius,
        radius * 2.0f, radius * 2.0f, radius,
        LerpColor(UI_COLOR_KNOB, UiColor(255, 255, 255, 255), hover));

    return changed;
}

bool UiToggle(UiContext* ui, uint32_t id, float x, float y, bool* value)
{
    float width = UiScaled(ui, 40.0f);
    float height = UiScaled(ui, 22.0f);
    float knobRadius = height * 0.5f - UiScaled(ui, 3.0f);

    bool hovered = ui->activeId == 0 && MouseInside(ui, x, y, width, height);
    bool changed = hovered && ui->mousePressed;
    if (changed)
    {
        *value = !*value;
    }

    float state = UiAnimate(ui, id, *value);
    float hover = UiAnimate(ui, id ^ 0x40000000u, hovered);

    uint32_t off = LerpColor(UI_COLOR_TRACK, UI_COLOR_BUTTON_HOT, hover * 0.6f);
    UiRect(ui, x, y, width, height, height * 0.5f,
        LerpColor(off, UI_COLOR_ACCENT, state));

    float knobTravel = width - height;
    float knobX = x + height * 0.5f + knobTravel * state;
    UiRect(ui, knobX - knobRadius, y + height * 0.5f - knobRadius,
        knobRadius * 2.0f, knobRadius * 2.0f, knobRadius, UI_COLOR_KNOB);

    return changed;
}

bool UiSegmented(UiContext* ui, uint32_t id, float x, float y,
    float width, float height, const wchar_t* const* labels,
    int32_t count, int32_t* activeIndex)
{
    if (count <= 0)
    {
        return false;
    }

    float radius = UiScaled(ui, 8.0f);
    UiRect(ui, x, y, width, height, radius, UI_COLOR_TRACK);

    float inset = UiScaled(ui, 3.0f);
    float segmentWidth = (width - inset * 2.0f) / (float)count;
    bool changed = false;

    for (int32_t index = 0; index < count; ++index)
    {
        float segmentX = x + inset + segmentWidth * (float)index;
        float segmentY = y + inset;
        float segmentHeight = height - inset * 2.0f;

        bool active = *activeIndex == index;
        bool hovered = ui->activeId == 0
            && MouseInside(ui, segmentX, segmentY, segmentWidth, segmentHeight);
        if (hovered && ui->mousePressed && !active)
        {
            *activeIndex = index;
            active = true;
            changed = true;
        }

        float selection = UiAnimate(ui, id + (uint32_t)index, active);
        float hover = UiAnimate(ui, (id + (uint32_t)index) ^ 0x10000000u,
            hovered && !active);

        if (selection > 0.01f || hover > 0.01f)
        {
            uint32_t background = LerpColor(
                UiColor(58, 68, 88, 0), UiColor(58, 68, 88, 200), hover);
            background = LerpColor(background,
                UiColor(74, 96, 150, 255), selection);
            UiRect(ui, segmentX, segmentY, segmentWidth, segmentHeight,
                radius - inset > 0.0f ? radius - inset : 0.0f, background);
        }

        float textTop = segmentY + (segmentHeight - ui->font.lineHeight) * 0.5f;
        UiTextCentered(ui, segmentX + segmentWidth * 0.5f, textTop,
            LerpColor(UI_COLOR_TEXT_DIM, UI_COLOR_TEXT, selection),
            labels[index]);
    }

    return changed;
}

bool UiRadioRow(UiContext* ui, uint32_t id, float x, float y,
    float width, float height, const wchar_t* label, bool selected)
{
    bool hovered = ui->activeId == 0 && MouseInside(ui, x, y, width, height);
    float hover = UiAnimate(ui, id, hovered);
    float state = UiAnimate(ui, id ^ 0x20000000u, selected);

    uint32_t background = LerpColor(
        UiColor(42, 48, 62, 0), UiColor(58, 68, 88, 255), hover * 0.6f);
    if (selected)
    {
        background = LerpColor(background, UiColor(50, 62, 92, 255), 0.75f);
    }
    UiRect(ui, x, y, width, height, UiScaled(ui, 7.0f), background);

    // Радио-точка: кольцо и заполнение при выборе.
    float dotRadius = UiScaled(ui, 7.0f);
    float dotX = x + UiScaled(ui, 14.0f);
    float dotY = y + height * 0.5f;
    UiRect(ui, dotX - dotRadius, dotY - dotRadius,
        dotRadius * 2.0f, dotRadius * 2.0f, dotRadius,
        LerpColor(UI_COLOR_TRACK, UI_COLOR_ACCENT, state));
    float holeRadius = dotRadius - UiScaled(ui, 2.0f) * (1.0f + state);
    if (holeRadius > 0.0f && state < 0.999f)
    {
        UiRect(ui, dotX - holeRadius, dotY - holeRadius,
            holeRadius * 2.0f, holeRadius * 2.0f, holeRadius,
            LerpColor(UiColor(22, 26, 34, 255), UI_COLOR_ACCENT, state));
    }

    float textTop = y + (height - ui->font.lineHeight) * 0.5f;
    UiText(ui, x + UiScaled(ui, 28.0f), textTop,
        selected ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, label);

    return hovered && ui->mousePressed;
}
