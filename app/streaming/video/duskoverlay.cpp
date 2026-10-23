#include "duskoverlay.h"
#include "streaming/duskchannel.h"
#include "overlaymanager.h"
#include "path.h"
#include "streaming/session.h"

#include <QSettings>

#include <Limelight.h>

using namespace Overlay;

// Compact by design: this drops down over a game, so it should cover as
// little of it as possible while staying clickable.
#define HANDLE_SIZE     40
#define MENU_WIDTH      248
#define MENU_PAD        8
#define ROW_HEIGHT      32
#define FONT_SIZE       15
#define TITLE_FONT_SIZE 15

// Far enough that a click with a shaky hand is still a click.
#define DRAG_THRESHOLD  4

#define SETTINGS_KEY_X "dusk/overlayhandlex"
#define SETTINGS_KEY_Y "dusk/overlayhandley"

// Dusk's accent, so the overlay reads as part of the same app as the grid.
static const SDL_Color ACCENT    = {0xE8, 0xB3, 0x3A, 0xFF};
static const SDL_Color PANEL_BG  = {0x14, 0x16, 0x1C, 0xF0};
static const SDL_Color HANDLE_BG = {0x14, 0x16, 0x1C, 0xC8};
static const SDL_Color TEXT      = {0xEC, 0xEC, 0xEF, 0xFF};
static const SDL_Color TEXT_DARK = {0x12, 0x11, 0x0C, 0xFF};

Menu::Menu(OverlayManager* manager) :
    m_Manager(manager),
    m_MenuOpen(false),
    m_Page(PageMain),
    m_Selected(0),
    m_HandleHot(false),
    m_Engaged(false),
    m_RestoreCapture(false),
    m_HandleX(0.975f),
    m_HandleY(0.025f),
    m_VideoX0(0.0f),
    m_VideoY0(0.0f),
    m_VideoX1(1.0f),
    m_VideoY1(1.0f),
    m_Dragging(false),
    m_DragMoved(false),
    m_DragOffsetX(0),
    m_DragOffsetY(0),
    m_WindowWidth(1920),
    m_WindowHeight(1080),
    m_Font(nullptr),
    m_TitleFont(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    loadPosition();
}

Menu::~Menu()
{
    if (m_Font != nullptr) {
        TTF_CloseFont(m_Font);
    }
    if (m_TitleFont != nullptr) {
        TTF_CloseFont(m_TitleFont);
    }
}

void Menu::loadPosition()
{
    QSettings settings;
    bool okX = false, okY = false;
    float x = settings.value(SETTINGS_KEY_X, m_HandleX).toFloat(&okX);
    float y = settings.value(SETTINGS_KEY_Y, m_HandleY).toFloat(&okY);

    // Clamp rather than trust: a corrupt or stale value must not park the
    // handle off-screen where it can never be dragged back.
    if (okX) {
        m_HandleX = SDL_clamp(x, 0.0f, 1.0f);
    }
    if (okY) {
        m_HandleY = SDL_clamp(y, 0.0f, 1.0f);
    }
}

void Menu::savePosition()
{
    QSettings settings;
    settings.setValue(SETTINGS_KEY_X, m_HandleX);
    settings.setValue(SETTINGS_KEY_Y, m_HandleY);
}

void Menu::setWindowSize(int width, int height)
{
    if (width > 0 && height > 0) {
        m_WindowWidth = width;
        m_WindowHeight = height;
    }
}

void Menu::setVideoRect(int x, int y, int w, int h, int surfaceWidth, int surfaceHeight)
{
    if (w <= 0 || h <= 0 || surfaceWidth <= 0 || surfaceHeight <= 0) {
        return;
    }

    m_VideoX0 = SDL_clamp((float)x / surfaceWidth, 0.0f, 1.0f);
    m_VideoY0 = SDL_clamp((float)y / surfaceHeight, 0.0f, 1.0f);
    m_VideoX1 = SDL_clamp((float)(x + w) / surfaceWidth, m_VideoX0, 1.0f);
    m_VideoY1 = SDL_clamp((float)(y + h) / surfaceHeight, m_VideoY0, 1.0f);
}

void Menu::videoIn(int spaceW, int spaceH, int* x0, int* y0, int* x1, int* y1) const
{
    *x0 = (int)(m_VideoX0 * spaceW);
    *y0 = (int)(m_VideoY0 * spaceH);
    *x1 = (int)(m_VideoX1 * spaceW);
    *y1 = (int)(m_VideoY1 * spaceH);
}

void Menu::anchorIn(int spaceW, int spaceH, int itemW, int itemH, int* x, int* y) const
{
    // Anchored inside the video, not the window. When the host's aspect
    // ratio does not match the window there are bars down the side, and
    // while the stream holds the mouse the pointer cannot leave the video
    // image — so a handle sitting in a bar is one the user can see and
    // never reach. Hovering it is what frees the cursor, so being out
    // there also locks itself out: the hotkey becomes the only way in.
    int x0, y0, x1, y1;
    videoIn(spaceW, spaceH, &x0, &y0, &x1, &y1);

    *x = SDL_clamp(x0 + (int)(m_HandleX * (x1 - x0)), x0, SDL_max(x0, x1 - itemW));
    *y = SDL_clamp(y0 + (int)(m_HandleY * (y1 - y0)), y0, SDL_max(y0, y1 - itemH));
}

bool Menu::openFont()
{
    if (m_Font != nullptr && m_TitleFont != nullptr) {
        return true;
    }

    if (m_FontData.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Dusk overlay font failed to load");
        return false;
    }

    // The RW ops take ownership (freesrc = 1), so each font needs its own.
    // m_FontData must outlive both, which it does as a member.
    if (m_Font == nullptr) {
        m_Font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                1, FONT_SIZE);
    }
    if (m_TitleFont == nullptr) {
        m_TitleFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                     1, TITLE_FONT_SIZE);
    }

    if (m_Font == nullptr || m_TitleFont == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_OpenFont() failed for Dusk overlay: %s", TTF_GetError());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------- geometry

int Menu::menuWidth() const
{
    return MENU_WIDTH;
}

int Menu::rowCount() const
{
    // The display page carries a Back row on the end.
    return m_Page == PageMain ? rowOf(ActionMax) : k_DisplayCount + 1;
}

bool Menu::actionShown(int action) const
{
    if (action == ActionOpenDusk) {
        Session* session = Session::get();
        return session != nullptr && session->getDuskChannel() != nullptr;
    }
    return true;
}

int Menu::actionAt(int row) const
{
    for (int action = 0; action < ActionMax; action++) {
        if (!actionShown(action)) {
            continue;
        }
        if (row-- == 0) {
            return action;
        }
    }
    return ActionMax;
}

int Menu::rowOf(int action) const
{
    int row = 0;
    for (int i = 0; i < action; i++) {
        if (actionShown(i)) {
            row++;
        }
    }
    return row;
}

int Menu::menuHeight() const
{
    return (MENU_PAD * 2) + (rowCount() * ROW_HEIGHT);
}

void Menu::handleOrigin(int displayWidth, int displayHeight, int surfaceWidth,
                        int surfaceHeight, int* x, int* y)
{
    anchorIn(displayWidth, displayHeight, surfaceWidth, surfaceHeight, x, y);
}

void Menu::menuOrigin(int displayWidth, int displayHeight, int surfaceWidth,
                      int surfaceHeight, int* x, int* y)
{
    // A dropdown: pinned under the handle and right-aligned with it, then
    // clamped so a handle in a corner does not push the menu off-screen.
    int handleW = (int)((float)HANDLE_SIZE / m_WindowWidth * displayWidth);
    int handleH = (int)((float)HANDLE_SIZE / m_WindowHeight * displayHeight);
    int handleX, handleY;
    handleOrigin(displayWidth, displayHeight, handleW, handleH, &handleX, &handleY);

    int x0, y0, x1, y1;
    videoIn(displayWidth, displayHeight, &x0, &y0, &x1, &y1);

    *x = SDL_clamp(handleX + handleW - surfaceWidth, x0, SDL_max(x0, x1 - surfaceWidth));
    *y = SDL_clamp(handleY + handleH + 6, y0, SDL_max(y0, y1 - surfaceHeight));
}

bool Menu::pointInHandle(int x, int y) const
{
    int hx, hy;
    anchorIn(m_WindowWidth, m_WindowHeight, HANDLE_SIZE, HANDLE_SIZE, &hx, &hy);
    return x >= hx && x < hx + HANDLE_SIZE && y >= hy && y < hy + HANDLE_SIZE;
}

int Menu::itemAtPoint(int x, int y) const
{
    int mx, my;
    const_cast<Menu*>(this)->menuOrigin(m_WindowWidth, m_WindowHeight,
                                        menuWidth(), menuHeight(), &mx, &my);

    int localX = x - mx;
    int localY = y - my - MENU_PAD;
    if (localX < 0 || localX >= menuWidth() || localY < 0) {
        return -1;
    }

    int index = localY / ROW_HEIGHT;
    return index < rowCount() ? index : -1;
}

// ------------------------------------------------------------------- state

void Menu::setEngaged(bool engaged)
{
    if (m_Engaged == engaged) {
        return;
    }

    Session* session = Session::s_ActiveSession;
    if (session == nullptr || session->m_InputHandler == nullptr) {
        return;
    }

    m_Engaged = engaged;

    if (engaged) {
        // Releasing capture on hover, not just on open, is what puts the
        // cursor back under the user's hand while they reach for the
        // handle. Remember what we interrupted so we restore their state
        // rather than assuming they were captured.
        m_RestoreCapture = session->m_InputHandler->isCaptureActive();

        // A stream holding the mouse in relative mode has no pointer
        // position, so nothing can be hovered and the hotkey is the only
        // way in. Coming out of it the cursor reappears wherever it was
        // parked before the stream started — which is why the handle could
        // be seen and still not be clicked or dragged. Put the cursor on
        // the handle instead, so one keypress hands the overlay to the
        // mouse rather than making the whole thing keyboard-only.
        bool relativeCapture = m_RestoreCapture
                               && !session->m_InputHandler->overlayPointerUsable();

        if (m_RestoreCapture) {
            session->m_InputHandler->setCaptureActive(false);
        }

        if (relativeCapture) {
            int hx, hy;
            anchorIn(m_WindowWidth, m_WindowHeight, HANDLE_SIZE, HANDLE_SIZE, &hx, &hy);
            session->m_InputHandler->warpCursorTo(hx + HANDLE_SIZE / 2,
                                                  hy + HANDLE_SIZE / 2);
            m_HandleHot = true;
            repaintHandle();
        }
    }
    else if (m_RestoreCapture) {
        m_RestoreCapture = false;
        session->m_InputHandler->setCaptureActive(true);
    }
}

void Menu::sendHostHotkey(short keyCode)
{
    // Modifiers are pressed as real keys as well as flagged: a host
    // watching for a chord needs to see them held, not merely described.
    const char mods = MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT;

    LiSendKeyboardEvent(0xA2 /* VK_LCONTROL */, KEY_ACTION_DOWN, MODIFIER_CTRL);
    LiSendKeyboardEvent(0xA4 /* VK_LMENU */, KEY_ACTION_DOWN, MODIFIER_CTRL | MODIFIER_ALT);
    LiSendKeyboardEvent(0xA0 /* VK_LSHIFT */, KEY_ACTION_DOWN, mods);

    LiSendKeyboardEvent(keyCode, KEY_ACTION_DOWN, mods);
    LiSendKeyboardEvent(keyCode, KEY_ACTION_UP, mods);

    LiSendKeyboardEvent(0xA0, KEY_ACTION_UP, MODIFIER_CTRL | MODIFIER_ALT);
    LiSendKeyboardEvent(0xA4, KEY_ACTION_UP, MODIFIER_CTRL);
    LiSendKeyboardEvent(0xA2, KEY_ACTION_UP, 0);
}

void Menu::switchHostDisplay(int index)
{
    // Sunshine listens for Ctrl+Alt+Shift+F1..F12 on the host.
    sendHostHotkey(0x70 /* VK_F1 */ + index);
}

void Menu::setMenuOpen(bool open)
{
    if (m_MenuOpen == open) {
        return;
    }

    m_MenuOpen = open;
    if (open) {
        // Always open on the first item. Resuming where the selection was
        // left invites accidentally activating Disconnect on a blind press.
        m_Selected = 0;
    }

    if (open) {
        m_Page = PageMain;
    }

    // Hand the real cursor back while the menu is up. This is the whole
    // reason there is no cursor of our own to go wrong.
    setEngaged(open || m_HandleHot);

    m_Manager->setOverlayState(OverlayMenu, open);
    if (open) {
        repaintMenu();
    }
    repaintHandle();
}

void Menu::refresh()
{
    m_Manager->setOverlayState(OverlayHandle, true);
    repaintHandle();
    if (m_MenuOpen) {
        repaintMenu();
    }
}

const char* Menu::labelFor(int index) const
{
    Session* session = Session::get();

    if (m_Page == PageDisplays) {
        static char label[24];
        if (index >= k_DisplayCount) {
            return "Back";
        }
        SDL_snprintf(label, sizeof(label), "Display %d", index + 1);
        return label;
    }

    switch (actionAt(index)) {
    case ActionStats:
        return (session != nullptr &&
                session->getOverlayManager().isOverlayEnabled(OverlayDebug))
                   ? "Hide performance stats"
                   : "Show performance stats";
    case ActionSwitchDisplay: return "Switch display";
    case ActionFullScreen:   return "Toggle full screen";
    case ActionReleaseMouse: return "Release the mouse";
    case ActionMinimize:     return "Minimise";
    case ActionOpenDusk:     return "Open Dusk";
    case ActionDisconnect:   return "Disconnect";
    case ActionEndSession:   return "End session";
    default:                 return "";
    }
}

void Menu::activate(int index)
{
    Session* session = Session::get();

    if (m_Page == PageDisplays) {
        if (index >= k_DisplayCount) {
            m_Page = PageMain;
            m_Selected = rowOf(ActionSwitchDisplay);
            repaintMenu();
            return;
        }

        switchHostDisplay(index);
        setMenuOpen(false);
        return;
    }

    switch (actionAt(index)) {
    case ActionStats:
        if (session != nullptr) {
            OverlayManager& overlays = session->getOverlayManager();
            overlays.setOverlayState(OverlayDebug, !overlays.isOverlayEnabled(OverlayDebug));
        }
        setMenuOpen(false);
        break;

    case ActionSwitchDisplay:
        m_Page = PageDisplays;
        m_Selected = 0;
        repaintMenu();
        break;

    case ActionFullScreen:
        setMenuOpen(false);
        if (Session::s_ActiveSession != nullptr) {
            Session::s_ActiveSession->toggleFullscreen();
        }
        break;

    case ActionReleaseMouse:
        // Close first, then leave it released: the close would otherwise
        // restore the capture we were just asked to drop.
        setMenuOpen(false);
        m_RestoreCapture = false;
        if (Session::s_ActiveSession != nullptr &&
            Session::s_ActiveSession->m_InputHandler != nullptr) {
            Session::s_ActiveSession->m_InputHandler->setCaptureActive(false);
        }
        break;

    case ActionMinimize:
        setMenuOpen(false);
        if (Session::s_ActiveSession != nullptr &&
            Session::s_ActiveSession->m_Window != nullptr) {
            SDL_MinimizeWindow(Session::s_ActiveSession->m_Window);
        }
        break;

    case ActionOpenDusk:
        // Dusk raises its window; getting the stream out of the way is
        // ours to do. Leave the mouse free so it can reach Dusk, and step
        // out of full screen's way, which would otherwise cover the window
        // Dusk just raised.
        setMenuOpen(false);
        m_RestoreCapture = false;
        if (Session::s_ActiveSession != nullptr) {
            if (Session::s_ActiveSession->m_InputHandler != nullptr) {
                Session::s_ActiveSession->m_InputHandler->setCaptureActive(false);
            }
            if (Session::s_ActiveSession->m_Window != nullptr &&
                (SDL_GetWindowFlags(Session::s_ActiveSession->m_Window) & SDL_WINDOW_FULLSCREEN)) {
                SDL_MinimizeWindow(Session::s_ActiveSession->m_Window);
            }
        }
        if (session != nullptr && session->getDuskChannel() != nullptr) {
            session->getDuskChannel()->sendOverlayAction("settings");
        }
        break;

    case ActionDisconnect:
        setMenuOpen(false);
        // Tell Dusk why the stream is about to end, then end it anyway.
        // Dusk answers with a disconnect of its own, but the button must
        // not depend on a round trip to a process that may be wedged.
        if (session != nullptr && session->getDuskChannel() != nullptr) {
            session->getDuskChannel()->sendOverlayAction("disconnect");
        }
        {
            // Same path as the quit key combo, so there is one teardown.
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_QUIT;
            event.quit.timestamp = SDL_GetTicks();
            SDL_PushEvent(&event);
        }
        break;

    case ActionEndSession:
        setMenuOpen(false);
        // Disconnect leaves the host's session running for the next stream
        // to resume. This closes it too, through the same path as the
        // quit-and-exit combo, so it works whether or not Dusk is listening.
        if (session != nullptr && session->getDuskChannel() != nullptr) {
            session->getDuskChannel()->sendOverlayAction("end_session");
        }
        if (session != nullptr) {
            session->setShouldExit(true);
        }
        {
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_QUIT;
            event.quit.timestamp = SDL_GetTicks();
            SDL_PushEvent(&event);
        }
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------------- input

bool Menu::handleKeyEvent(const SDL_KeyboardEvent* event)
{
    if (!m_MenuOpen) {
        return false;
    }

    // Swallow key-up too, so the host never sees a release for a press it
    // never got.
    if (event->state != SDL_PRESSED) {
        return true;
    }

    switch (event->keysym.sym) {
    case SDLK_UP:
    case SDLK_k:
        m_Selected = (m_Selected + rowCount() - 1) % rowCount();
        repaintMenu();
        break;

    case SDLK_DOWN:
    case SDLK_j:
        m_Selected = (m_Selected + 1) % rowCount();
        repaintMenu();
        break;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        activate(m_Selected);
        break;

    case SDLK_ESCAPE:
        if (m_Page == PageDisplays) {
            m_Page = PageMain;
            m_Selected = rowOf(ActionSwitchDisplay);
            repaintMenu();
        }
        else {
            setMenuOpen(false);
        }
        break;

    default:
        break;
    }

    return true;
}

bool Menu::handleMouseMotion(int x, int y, bool usable)
{
    if (!usable) {
        // Captured in relative mode: the coordinates are meaningless, so
        // hovering is not a thing that can happen. Say so rather than
        // hit-testing against a position that is not real.
        return false;
    }

    if (m_Dragging) {
        m_DragMoved = true;
        int x0, y0, x1, y1;
        videoIn(m_WindowWidth, m_WindowHeight, &x0, &y0, &x1, &y1);
        m_HandleX = SDL_clamp((float)(x - m_DragOffsetX - x0) / SDL_max(1, x1 - x0), 0.0f, 1.0f);
        m_HandleY = SDL_clamp((float)(y - m_DragOffsetY - y0) / SDL_max(1, y1 - y0), 0.0f, 1.0f);
        repaintHandle();
        if (m_MenuOpen) {
            repaintMenu();
        }
        return true;
    }

    if (m_MenuOpen) {
        int hovered = itemAtPoint(x, y);
        if (hovered >= 0 && hovered != m_Selected) {
            m_Selected = hovered;
            repaintMenu();
        }
        // The whole pointer belongs to the menu while it is open, otherwise
        // aiming at a button would also be aiming in the game behind it.
        return true;
    }

    bool hot = pointInHandle(x, y);
    if (hot != m_HandleHot) {
        m_HandleHot = hot;
        // Engaging on hover is what brings the system cursor back while
        // the user reaches for the handle; without it they are aiming at
        // a button with nothing on screen to aim with.
        setEngaged(hot || m_MenuOpen);
        repaintHandle();
    }

    // Motion elsewhere is the host's. Swallowing it would make the stream
    // feel broken for the sake of a button nobody is touching.
    return hot;
}

bool Menu::handleMouseButton(int button, bool pressed, int x, int y, bool usable)
{
    if (!usable) {
        return false;
    }

    if (button != SDL_BUTTON_LEFT) {
        // Right-click closes; other buttons are the host's business.
        if (m_MenuOpen && pressed) {
            setMenuOpen(false);
            return true;
        }
        return m_MenuOpen;
    }

    if (pressed) {
        if (pointInHandle(x, y)) {
            int hx, hy;
            anchorIn(m_WindowWidth, m_WindowHeight, HANDLE_SIZE, HANDLE_SIZE, &hx, &hy);
            m_Dragging = true;
            m_DragMoved = false;
            m_DragOffsetX = x - hx;
            m_DragOffsetY = y - hy;
            return true;
        }

        if (m_MenuOpen) {
            int item = itemAtPoint(x, y);
            if (item >= 0) {
                activate(item);
            }
            else {
                // Clicking away closes, which is what every menu does.
                setMenuOpen(false);
            }
            return true;
        }

        return false;
    }

    // Released
    if (m_Dragging) {
        bool moved = m_DragMoved;
        m_Dragging = false;
        if (moved) {
            savePosition();
        }
        else {
            // A press and release without movement is a click, not a drag.
            toggleMenu();
        }
        return true;
    }

    return m_MenuOpen;
}

// ----------------------------------------------------------------- drawing

void Menu::drawRect(SDL_Surface* surface, int x, int y, int w, int h, SDL_Color color)
{
    SDL_Rect rect = { x, y, w, h };
    SDL_FillRect(surface, &rect, SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a));
}

void Menu::drawText(SDL_Surface* surface, TTF_Font* font, const char* text,
                    int x, int y, SDL_Color color)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    SDL_Surface* rendered = TTF_RenderUTF8_Blended(font, text, color);
    if (rendered == nullptr) {
        return;
    }

    // BLEND rather than the default NONE: the text carries its own alpha
    // and must composite onto the panel instead of stamping transparent
    // pixels over it.
    SDL_SetSurfaceBlendMode(rendered, SDL_BLENDMODE_BLEND);

    SDL_Rect dst = { x, y, rendered->w, rendered->h };
    SDL_BlitSurface(rendered, nullptr, surface, &dst);
    SDL_FreeSurface(rendered);
}

void Menu::repaintHandle()
{
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, HANDLE_SIZE, HANDLE_SIZE, 32,
                                                          SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return;
    }

    const bool lit = m_HandleHot || m_MenuOpen;

    drawRect(surface, 0, 0, HANDLE_SIZE, HANDLE_SIZE, lit ? ACCENT : HANDLE_BG);
    if (!lit) {
        drawRect(surface, 0, 0, HANDLE_SIZE, 2, ACCENT);
    }

    // Dusk's mark rather than a generic hamburger: a crescent with the sun
    // in its curve. Same geometry as scripts/make_icon.py, in unit
    // coordinates, so the button in-stream and the icon in the dock are
    // demonstrably the same drawing.
    const float inset = HANDLE_SIZE * 0.22f;
    const float span = HANDLE_SIZE - (inset * 2.0f);
    const SDL_Color moonInk = lit ? TEXT_DARK : TEXT;
    const SDL_Color sunInk = lit ? TEXT_DARK : ACCENT;

    struct Disc { float cx, cy, r; };
    const Disc crescent = { 0.469f, 0.500f, 0.359f };
    const Disc bite     = { 0.625f, 0.500f, 0.3125f };
    const Disc sun      = { 0.625f, 0.500f, 0.164f };

    auto inside = [&](const Disc& d, float x, float y) {
        const float dx = x - (inset + d.cx * span);
        const float dy = y - (inset + d.cy * span);
        return (dx * dx + dy * dy) <= (d.r * span) * (d.r * span);
    };

    // Sampled 3x3 per pixel: at this size a hard edge on a circle is a
    // staircase, and there is no antialiased fill to hand.
    for (int py = 0; py < HANDLE_SIZE; py++) {
        for (int px = 0; px < HANDLE_SIZE; px++) {
            int moonHits = 0, sunHits = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    const float x = px + (sx + 0.5f) / 3.0f;
                    const float y = py + (sy + 0.5f) / 3.0f;
                    if (inside(sun, x, y)) {
                        sunHits++;
                    }
                    else if (inside(crescent, x, y) && !inside(bite, x, y)) {
                        moonHits++;
                    }
                }
            }

            if (sunHits == 0 && moonHits == 0) {
                continue;
            }

            const bool isSun = sunHits >= moonHits;
            const SDL_Color ink = isSun ? sunInk : moonInk;
            const float a = (isSun ? sunHits : moonHits) / 9.0f;

            // Composited by hand against the known backdrop. SDL_FillRect
            // replaces rather than blends, so writing partial alpha
            // straight in would punch translucent notches along every
            // curve instead of softening it.
            const SDL_Color bg = lit ? ACCENT : HANDLE_BG;
            SDL_Color out;
            out.r = (Uint8)(ink.r * a + bg.r * (1.0f - a));
            out.g = (Uint8)(ink.g * a + bg.g * (1.0f - a));
            out.b = (Uint8)(ink.b * a + bg.b * (1.0f - a));
            out.a = (Uint8)(255 * a + bg.a * (1.0f - a));
            drawRect(surface, px, py, 1, 1, out);
        }
    }

    m_Manager->setOverlaySurface(OverlayHandle, surface);
}

void Menu::repaintMenu()
{
    if (!m_MenuOpen || !openFont()) {
        return;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, menuWidth(), menuHeight(), 32,
                                                          SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create Dusk overlay surface: %s", SDL_GetError());
        return;
    }

    // Partial alpha in the panel colour is what lets the stream show
    // through; the renderer blends the surface as a whole.
    drawRect(surface, 0, 0, menuWidth(), menuHeight(), PANEL_BG);
    drawRect(surface, 0, 0, menuWidth(), 2, ACCENT);

    for (int i = 0; i < rowCount(); i++) {
        const bool selected = (i == m_Selected);
        const int y = MENU_PAD + (i * ROW_HEIGHT);

        if (selected) {
            drawRect(surface, 0, y, menuWidth(), ROW_HEIGHT, ACCENT);
        }

        drawText(surface, m_Font, labelFor(i), MENU_PAD + 6, y + 7,
                 selected ? TEXT_DARK : TEXT);
    }

    m_Manager->setOverlaySurface(OverlayMenu, surface);
}
