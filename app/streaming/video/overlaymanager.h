#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    // Dusk's persistent handle and the menu it opens. Unlike the two above
    // these are not text: the surfaces are drawn by Overlay::Menu and
    // handed over with setOverlaySurface(), so the font path is skipped
    // entirely. Their positions are dynamic, so renderers ask Menu where to
    // put them instead of hardcoding a corner.
    OverlayHandle,
    OverlayMenu,
    OverlayMax
};

class Menu;

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

    /// Hand the manager a surface drawn by someone else.
    ///
    /// Takes ownership. This is the non-text path: the caller has already
    /// rendered pixels, so no font is involved and the text buffer is left
    /// alone.
    void setOverlaySurface(OverlayType type, SDL_Surface* surface);

    /// Say that a text overlay's pixels are being supplied from outside,
    /// so the font path leaves them alone.
    ///
    /// `OverlayHandle` and `OverlayMenu` are always drawn this way and need
    /// no flag. This is for `OverlayStatusUpdate`, which Dusk borrows for
    /// its notices: it has to go back to being Moonlight's own status text
    /// when the notice ends, so which of the two owns it is a state rather
    /// than a property of the type.
    void setOverlaySurfaceOwned(OverlayType type, bool owned);

    Menu& getMenu();

private:
    void notifyOverlayUpdated(OverlayType type);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct {
        bool enabled;
        /// Pixels come from setOverlaySurface() rather than the font path.
        bool surfaceOwned;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
    Menu* m_Menu;
};

}
