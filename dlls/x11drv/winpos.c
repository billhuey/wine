/*
 * Window position related functions.
 *
 * Copyright 1993, 1994, 1995, 2001 Alexandre Julliard
 * Copyright 1995, 1996, 1999 Alex Korobka
 */

#include "config.h"

#include "ts_xlib.h"
#include "ts_xutil.h"
#include "ts_shape.h"

#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"

#include "x11drv.h"
#include "hook.h"
#include "win.h"
#include "winpos.h"
#include "region.h"
#include "dce.h"
#include "cursoricon.h"
#include "nonclient.h"
#include "message.h"

#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(x11drv);

#define SWP_AGG_NOGEOMETRYCHANGE \
    (SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE)
#define SWP_AGG_NOPOSCHANGE \
    (SWP_AGG_NOGEOMETRYCHANGE | SWP_NOZORDER)
#define SWP_AGG_STATUSFLAGS \
    (SWP_AGG_NOPOSCHANGE | SWP_FRAMECHANGED | SWP_HIDEWINDOW | SWP_SHOWWINDOW)

#define SWP_EX_NOCOPY       0x0001
#define SWP_EX_PAINTSELF    0x0002
#define SWP_EX_NONCLIENT    0x0004

#define HAS_THICKFRAME(style,exStyle) \
    (((style) & WS_THICKFRAME) && \
     !(((style) & (WS_DLGFRAME|WS_BORDER)) == WS_DLGFRAME))

#define ON_LEFT_BORDER(hit) \
 (((hit) == HTLEFT) || ((hit) == HTTOPLEFT) || ((hit) == HTBOTTOMLEFT))
#define ON_RIGHT_BORDER(hit) \
 (((hit) == HTRIGHT) || ((hit) == HTTOPRIGHT) || ((hit) == HTBOTTOMRIGHT))
#define ON_TOP_BORDER(hit) \
 (((hit) == HTTOP) || ((hit) == HTTOPLEFT) || ((hit) == HTTOPRIGHT))
#define ON_BOTTOM_BORDER(hit) \
 (((hit) == HTBOTTOM) || ((hit) == HTBOTTOMLEFT) || ((hit) == HTBOTTOMRIGHT))


/***********************************************************************
 *		clip_children
 *
 * Clip all children of a given window out of the visible region
 */
static void clip_children( WND *win, WND *last, HRGN hrgn, int whole_window )
{
    WND *ptr;
    HRGN rectRgn;
    int x, y;

    /* first check if we have anything to do */
    for (ptr = win->child; ptr && ptr != last; ptr = ptr->next)
        if (ptr->dwStyle & WS_VISIBLE) break;
    if (!ptr || ptr == last) return; /* no children to clip */

    if (whole_window)
    {
        x = win->rectWindow.left - win->rectClient.left;
        y = win->rectWindow.top - win->rectClient.top;
    }
    else x = y = 0;

    rectRgn = CreateRectRgn( 0, 0, 0, 0 );
    while (ptr && ptr != last)
    {
        if (ptr->dwStyle & WS_VISIBLE)
        {
            SetRectRgn( rectRgn, ptr->rectWindow.left + x, ptr->rectWindow.top + y,
                        ptr->rectWindow.right + x, ptr->rectWindow.bottom + y );
            if (CombineRgn( hrgn, hrgn, rectRgn, RGN_DIFF ) == NULLREGION)
                break;  /* no need to go on, region is empty */
        }
        ptr = ptr->next;
    }
    DeleteObject( rectRgn );
}


/***********************************************************************
 *		get_visible_region
 *
 * Compute the visible region of a window
 */
static HRGN get_visible_region( WND *win, WND *top, UINT flags, int mode )
{
    HRGN rgn;
    RECT rect;
    int xoffset, yoffset;
    X11DRV_WND_DATA *data = win->pDriverData;

    if (flags & DCX_WINDOW)
    {
        xoffset = win->rectWindow.left;
        yoffset = win->rectWindow.top;
    }
    else
    {
        xoffset = win->rectClient.left;
        yoffset = win->rectClient.top;
    }

    if (flags & DCX_PARENTCLIP)
        GetClientRect( win->parent->hwndSelf, &rect );
    else if (flags & DCX_WINDOW)
        rect = data->whole_rect;
    else
        rect = win->rectClient;

    /* vis region is relative to the start of the client/window area */
    OffsetRect( &rect, -xoffset, -yoffset );

    if (!(rgn = CreateRectRgn( rect.left, rect.top, rect.right, rect.bottom ))) return 0;

    if ((flags & DCX_CLIPCHILDREN) && (mode != ClipByChildren))
    {
        /* we need to clip children by hand */
        clip_children( win, NULL, rgn, (flags & DCX_WINDOW) );
    }

    if (top && top != win)  /* need to clip siblings of ancestors */
    {
        WND *ptr = win;

        OffsetRgn( rgn, xoffset, yoffset );
        for (;;)
        {
            if (ptr->dwStyle & WS_CLIPSIBLINGS) clip_children( ptr->parent, ptr, rgn, FALSE );
            if (ptr == top) break;
            ptr = ptr->parent;
            OffsetRgn( rgn, ptr->rectClient.left, ptr->rectClient.top );
            xoffset += ptr->rectClient.left;
            yoffset += ptr->rectClient.top;
        }
        /* make it relative to the target window again */
        OffsetRgn( rgn, -xoffset, -yoffset );
    }

    return rgn;
}


/***********************************************************************
 *		X11DRV_GetDC (X11DRV.@)
 *
 * Set the drawable, origin and dimensions for the DC associated to
 * a given window.
 */
BOOL X11DRV_GetDC( HWND hwnd, HDC hdc, HRGN hrgn, DWORD flags )
{
    WND *win = WIN_FindWndPtr( hwnd );
    WND *ptr, *top;
    X11DRV_WND_DATA *data = win->pDriverData;
    Drawable drawable;
    int org_x, org_y, mode = IncludeInferiors;

    /* don't clip siblings if using parent clip region */
    if (flags & DCX_PARENTCLIP) flags &= ~DCX_CLIPSIBLINGS;

    /* find the top parent in the hierarchy that isn't clipping siblings */
    top = NULL;
    for (ptr = win->parent; ptr && ptr->parent; ptr = ptr->parent)
        if (!(ptr->dwStyle & WS_CLIPSIBLINGS)) top = ptr;

    if (!top && !(flags & DCX_CLIPSIBLINGS)) top = win;

    if (top)
    {
        org_x = org_y = 0;
        if (flags & DCX_WINDOW)
        {
            org_x = win->rectWindow.left - win->rectClient.left;
            org_y = win->rectWindow.top - win->rectClient.top;
        }
        for (ptr = win; ptr != top->parent; ptr = ptr->parent)
        {
            org_x += ptr->rectClient.left;
            org_y += ptr->rectClient.top;
        }
        /* have to use the parent so that we include siblings */
        if (top->parent) drawable = get_client_window( top->parent );
        else drawable = root_window;
    }
    else
    {
        if (IsIconic( hwnd ))
        {
            drawable = data->icon_window ? data->icon_window : data->whole_window;
            org_x = 0;
            org_y = 0;
        }
        else if (flags & DCX_WINDOW)
        {
            drawable = data->whole_window;
            org_x = win->rectWindow.left - data->whole_rect.left;
            org_y = win->rectWindow.top - data->whole_rect.top;
        }
        else
        {
            drawable = data->client_window;
            org_x = 0;
            org_y = 0;
            if (flags & DCX_CLIPCHILDREN) mode = ClipByChildren;  /* can use X11 clipping */
        }
    }

    X11DRV_SetDrawable( hdc, drawable, mode, org_x, org_y );

    if (flags & (DCX_EXCLUDERGN | DCX_INTERSECTRGN) ||
        SetHookFlags16( hdc, DCHF_VALIDATEVISRGN ))  /* DC was dirty */
    {
        /* need to recompute the visible region */
        HRGN visRgn = get_visible_region( win, top, flags, mode );

        if (flags & (DCX_EXCLUDERGN | DCX_INTERSECTRGN))
            CombineRgn( visRgn, visRgn, hrgn, (flags & DCX_INTERSECTRGN) ? RGN_AND : RGN_DIFF );

        /* make it relative to the drawable origin */
        OffsetRgn( visRgn, org_x, org_y );
        SelectVisRgn16( hdc, visRgn );
        DeleteObject( visRgn );
    }

    WIN_ReleaseWndPtr( win );
    return TRUE;
}



/***********************************************************************
 *           SWP_DoWinPosChanging
 */
static BOOL SWP_DoWinPosChanging( WINDOWPOS* pWinpos, RECT* pNewWindowRect, RECT* pNewClientRect )
{
    WND *wndPtr;

    /* Send WM_WINDOWPOSCHANGING message */

    if (!(pWinpos->flags & SWP_NOSENDCHANGING))
        SendMessageA( pWinpos->hwnd, WM_WINDOWPOSCHANGING, 0, (LPARAM)pWinpos );

    if (!(wndPtr = WIN_FindWndPtr( pWinpos->hwnd ))) return FALSE;

    /* Calculate new position and size */

    *pNewWindowRect = wndPtr->rectWindow;
    *pNewClientRect = (wndPtr->dwStyle & WS_MINIMIZE) ? wndPtr->rectWindow
                                                    : wndPtr->rectClient;

    if (!(pWinpos->flags & SWP_NOSIZE))
    {
        pNewWindowRect->right  = pNewWindowRect->left + pWinpos->cx;
        pNewWindowRect->bottom = pNewWindowRect->top + pWinpos->cy;
    }
    if (!(pWinpos->flags & SWP_NOMOVE))
    {
        pNewWindowRect->left    = pWinpos->x;
        pNewWindowRect->top     = pWinpos->y;
        pNewWindowRect->right  += pWinpos->x - wndPtr->rectWindow.left;
        pNewWindowRect->bottom += pWinpos->y - wndPtr->rectWindow.top;

        OffsetRect( pNewClientRect, pWinpos->x - wndPtr->rectWindow.left,
                                    pWinpos->y - wndPtr->rectWindow.top );
    }
    pWinpos->flags |= SWP_NOCLIENTMOVE | SWP_NOCLIENTSIZE;
    WIN_ReleaseWndPtr( wndPtr );
    return TRUE;
}

/***********************************************************************
 *           SWP_DoNCCalcSize
 */
static UINT SWP_DoNCCalcSize( WND* wndPtr, WINDOWPOS* pWinpos,
                              RECT* pNewWindowRect, RECT* pNewClientRect )
{
    UINT wvrFlags = 0;

      /* Send WM_NCCALCSIZE message to get new client area */
    if( (pWinpos->flags & (SWP_FRAMECHANGED | SWP_NOSIZE)) != SWP_NOSIZE )
    {
         wvrFlags = WINPOS_SendNCCalcSize( pWinpos->hwnd, TRUE, pNewWindowRect,
                                    &wndPtr->rectWindow, &wndPtr->rectClient,
                                    pWinpos, pNewClientRect );
         /* FIXME: WVR_ALIGNxxx */

         if( pNewClientRect->left != wndPtr->rectClient.left ||
             pNewClientRect->top != wndPtr->rectClient.top )
             pWinpos->flags &= ~SWP_NOCLIENTMOVE;

         if( (pNewClientRect->right - pNewClientRect->left !=
              wndPtr->rectClient.right - wndPtr->rectClient.left) ||
             (pNewClientRect->bottom - pNewClientRect->top !=
              wndPtr->rectClient.bottom - wndPtr->rectClient.top) )
             pWinpos->flags &= ~SWP_NOCLIENTSIZE;
    }
    else
      if( !(pWinpos->flags & SWP_NOMOVE) && (pNewClientRect->left != wndPtr->rectClient.left ||
                                             pNewClientRect->top != wndPtr->rectClient.top) )
            pWinpos->flags &= ~SWP_NOCLIENTMOVE;
    return wvrFlags;
}

/***********************************************************************
 *           SWP_DoOwnedPopups
 *
 * fix Z order taking into account owned popups -
 * basically we need to maintain them above the window that owns them
 *
 * FIXME: hide/show owned popups when owner visibility changes.
 */
static HWND SWP_DoOwnedPopups(WND* pDesktop, WND* wndPtr, HWND hwndInsertAfter, WORD flags)
{
    WND *w = WIN_LockWndPtr(pDesktop->child);

    WARN("(%04x) hInsertAfter = %04x\n", wndPtr->hwndSelf, hwndInsertAfter );

    if( (wndPtr->dwStyle & WS_POPUP) && wndPtr->owner )
    {
        /* make sure this popup stays above the owner */

        HWND hwndLocalPrev = HWND_TOP;

        if( hwndInsertAfter != HWND_TOP )
        {
            while( w && w != wndPtr->owner )
            {
                if (w != wndPtr) hwndLocalPrev = w->hwndSelf;
                if( hwndLocalPrev == hwndInsertAfter ) break;
                WIN_UpdateWndPtr(&w,w->next);
            }
            hwndInsertAfter = hwndLocalPrev;
        }
    }
    else if( wndPtr->dwStyle & WS_CHILD )
        goto END;

    WIN_UpdateWndPtr(&w, pDesktop->child);

    while( w )
    {
        if( w == wndPtr ) break;

        if( (w->dwStyle & WS_POPUP) && w->owner == wndPtr )
        {
            SetWindowPos(w->hwndSelf, hwndInsertAfter, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_DEFERERASE);
            hwndInsertAfter = w->hwndSelf;
        }
        WIN_UpdateWndPtr(&w, w->next);
    }

END:
    WIN_ReleaseWndPtr(w);
    return hwndInsertAfter;
}


/* fix redundant flags and values in the WINDOWPOS structure */
static BOOL fixup_flags( WINDOWPOS *winpos )
{
    WND *wndPtr = WIN_FindWndPtr( winpos->hwnd );
    BOOL ret = TRUE;

    if (!wndPtr) return FALSE;

    if (wndPtr->dwStyle & WS_VISIBLE) winpos->flags &= ~SWP_SHOWWINDOW;
    else
    {
        winpos->flags &= ~SWP_HIDEWINDOW;
        if (!(winpos->flags & SWP_SHOWWINDOW)) winpos->flags |= SWP_NOREDRAW;
    }

    if (winpos->cx < 0) winpos->cx = 0;
    if (winpos->cy < 0) winpos->cy = 0;

    if ((wndPtr->rectWindow.right - wndPtr->rectWindow.left == winpos->cx) &&
        (wndPtr->rectWindow.bottom - wndPtr->rectWindow.top == winpos->cy))
        winpos->flags |= SWP_NOSIZE;    /* Already the right size */

    if ((wndPtr->rectWindow.left == winpos->x) && (wndPtr->rectWindow.top == winpos->y))
        winpos->flags |= SWP_NOMOVE;    /* Already the right position */

    if (winpos->hwnd == GetForegroundWindow())
        winpos->flags |= SWP_NOACTIVATE;   /* Already active */
    else if ((wndPtr->dwStyle & (WS_POPUP | WS_CHILD)) != WS_CHILD)
    {
        if (!(winpos->flags & SWP_NOACTIVATE)) /* Bring to the top when activating */
        {
            winpos->flags &= ~SWP_NOZORDER;
            winpos->hwndInsertAfter = HWND_TOP;
            goto done;
        }
    }

    /* Check hwndInsertAfter */

      /* FIXME: TOPMOST not supported yet */
    if ((winpos->hwndInsertAfter == HWND_TOPMOST) ||
        (winpos->hwndInsertAfter == HWND_NOTOPMOST)) winpos->hwndInsertAfter = HWND_TOP;

    /* hwndInsertAfter must be a sibling of the window */
    if ((winpos->hwndInsertAfter != HWND_TOP) && (winpos->hwndInsertAfter != HWND_BOTTOM))
    {
        WND* wnd = WIN_FindWndPtr(winpos->hwndInsertAfter);
        if (wnd)
        {
            if (wnd->parent != wndPtr->parent) ret = FALSE;
            else
            {
                /* don't need to change the Zorder of hwnd if it's already inserted
                 * after hwndInsertAfter or when inserting hwnd after itself.
                 */
                if ((wnd->next == wndPtr ) || (winpos->hwnd == winpos->hwndInsertAfter))
                    winpos->flags |= SWP_NOZORDER;
            }
            WIN_ReleaseWndPtr(wnd);
        }
    }
 done:
    WIN_ReleaseWndPtr(wndPtr);
    return ret;
}


/***********************************************************************
 *		SetWindowPos   (X11DRV.@)
 */
BOOL X11DRV_SetWindowPos( WINDOWPOS *winpos )
{
    WND *wndPtr;
    RECT newWindowRect, newClientRect;
    RECT oldWindowRect, oldClientRect;
    UINT wvrFlags = 0;
    BOOL bChangePos;

    TRACE( "hwnd %04x, swp (%i,%i)-(%i,%i) flags %08x\n",
           winpos->hwnd, winpos->x, winpos->y,
           winpos->x + winpos->cx, winpos->y + winpos->cy, winpos->flags);

    bChangePos = !(winpos->flags & SWP_WINE_NOHOSTMOVE);
    winpos->flags &= ~SWP_WINE_NOHOSTMOVE;

    /* Check window handle */
    if (winpos->hwnd == GetDesktopWindow()) return FALSE;

    /* Fix redundant flags */
    if (!fixup_flags( winpos )) return FALSE;

    SWP_DoWinPosChanging( winpos, &newWindowRect, &newClientRect );

    if (!(wndPtr = WIN_FindWndPtr( winpos->hwnd ))) return FALSE;

    TRACE("\tcurrent (%i,%i)-(%i,%i), style %08x\n",
          wndPtr->rectWindow.left, wndPtr->rectWindow.top,
          wndPtr->rectWindow.right, wndPtr->rectWindow.bottom, (unsigned)wndPtr->dwStyle );

    if((winpos->flags & (SWP_NOZORDER | SWP_HIDEWINDOW | SWP_SHOWWINDOW)) != SWP_NOZORDER)
    {
        if( wndPtr->parent->hwndSelf == GetDesktopWindow() )
            winpos->hwndInsertAfter = SWP_DoOwnedPopups( wndPtr->parent, wndPtr,
                                                         winpos->hwndInsertAfter, winpos->flags );
    }

    /* Common operations */

    wvrFlags = SWP_DoNCCalcSize( wndPtr, winpos, &newWindowRect, &newClientRect );

    if(!(winpos->flags & SWP_NOZORDER) && winpos->hwnd != winpos->hwndInsertAfter)
    {
        if ( WIN_UnlinkWindow( winpos->hwnd ) )
            WIN_LinkWindow( winpos->hwnd, winpos->hwndInsertAfter );
    }

    /* Reset active DCEs */

    if( (((winpos->flags & SWP_AGG_NOPOSCHANGE) != SWP_AGG_NOPOSCHANGE) &&
         wndPtr->dwStyle & WS_VISIBLE) ||
        (winpos->flags & (SWP_HIDEWINDOW | SWP_SHOWWINDOW)) )
    {
        RECT rect;

        UnionRect(&rect, &newWindowRect, &wndPtr->rectWindow);
        DCE_InvalidateDCE(wndPtr, &rect);
    }

    oldWindowRect = wndPtr->rectWindow;
    oldClientRect = wndPtr->rectClient;

    /* Find out if we have to redraw the whole client rect */

    if( oldClientRect.bottom - oldClientRect.top ==
        newClientRect.bottom - newClientRect.top ) wvrFlags &= ~WVR_VREDRAW;

    if( oldClientRect.right - oldClientRect.left ==
        newClientRect.right - newClientRect.left ) wvrFlags &= ~WVR_HREDRAW;

    /* FIXME: actually do something with WVR_VALIDRECTS */

    wndPtr->rectWindow = newWindowRect;
    wndPtr->rectClient = newClientRect;

    if (winpos->flags & SWP_SHOWWINDOW) wndPtr->dwStyle |= WS_VISIBLE;
    else if (winpos->flags & SWP_HIDEWINDOW)
    {
        /* clear the update region */
        RedrawWindow( winpos->hwnd, NULL, 0, RDW_VALIDATE | RDW_NOFRAME | RDW_NOERASE |
                                             RDW_NOINTERNALPAINT | RDW_ALLCHILDREN );
        wndPtr->dwStyle &= ~WS_VISIBLE;
    }

    if (get_whole_window(wndPtr))  /* don't do anything if X window not created yet */
    {
        Display *display = thread_display();

        wine_tsx11_lock();
        if (!(winpos->flags & SWP_SHOWWINDOW) && (winpos->flags & SWP_HIDEWINDOW))
        {
            if (!IsRectEmpty( &oldWindowRect ))
            {
                XUnmapWindow( display, get_whole_window(wndPtr) );
                TRACE( "unmapping win %x\n", winpos->hwnd );
            }
            else TRACE( "not unmapping zero size win %x\n", winpos->hwnd );
        }
        else if ((wndPtr->dwStyle & WS_VISIBLE) &&
                 !IsRectEmpty( &oldWindowRect ) && IsRectEmpty( &newWindowRect ))
        {
            /* resizing to zero size -> unmap */
            TRACE( "unmapping zero size win %x\n", winpos->hwnd );
            XUnmapWindow( display, get_whole_window(wndPtr) );
        }

        if (bChangePos)
            X11DRV_sync_whole_window_position( display, wndPtr, !(winpos->flags & SWP_NOZORDER) );
        else
        {
            struct x11drv_win_data *data = wndPtr->pDriverData;
            data->whole_rect = wndPtr->rectWindow;
            X11DRV_window_to_X_rect( wndPtr, &data->whole_rect );
        }

        if (X11DRV_sync_client_window_position( display, wndPtr ) ||
            (winpos->flags & SWP_FRAMECHANGED))
        {
            /* if we moved the client area, repaint the whole non-client window */
            XClearArea( display, get_whole_window(wndPtr), 0, 0, 0, 0, True );
        }
        if (winpos->flags & SWP_SHOWWINDOW)
        {
            if (!IsRectEmpty( &newWindowRect ))
            {
                XMapWindow( display, get_whole_window(wndPtr) );
                TRACE( "mapping win %x\n", winpos->hwnd );
            }
            else TRACE( "not mapping win %x, size is zero\n", winpos->hwnd );
        }
        else if ((wndPtr->dwStyle & WS_VISIBLE) &&
                 IsRectEmpty( &oldWindowRect ) && !IsRectEmpty( &newWindowRect ))
        {
            /* resizing from zero size to non-zero -> map */
            TRACE( "mapping non zero size win %x\n", winpos->hwnd );
            XMapWindow( display, get_whole_window(wndPtr) );
        }
        XFlush( display );  /* FIXME: should not be necessary */
        wine_tsx11_unlock();
    }

    WIN_ReleaseWndPtr(wndPtr);

    if (wvrFlags & WVR_REDRAW) RedrawWindow( winpos->hwnd, NULL, 0, RDW_INVALIDATE | RDW_ERASE );

    if (winpos->hwnd == CARET_GetHwnd())
    {
        if( winpos->flags & SWP_HIDEWINDOW )
            HideCaret(winpos->hwnd);
        else if (winpos->flags & SWP_SHOWWINDOW)
            ShowCaret(winpos->hwnd);
    }

    if (!(winpos->flags & SWP_NOACTIVATE))
        WINPOS_ChangeActiveWindow( winpos->hwnd, FALSE );

      /* And last, send the WM_WINDOWPOSCHANGED message */

    TRACE("\tstatus flags = %04x\n", winpos->flags & SWP_AGG_STATUSFLAGS);

    if (((winpos->flags & SWP_AGG_STATUSFLAGS) != SWP_AGG_NOPOSCHANGE) &&
          !(winpos->flags & SWP_NOSENDCHANGING))
        SendMessageA( winpos->hwnd, WM_WINDOWPOSCHANGED, 0, (LPARAM)winpos );

    return TRUE;
}


/***********************************************************************
 *           WINPOS_FindIconPos
 *
 * Find a suitable place for an iconic window.
 */
static POINT WINPOS_FindIconPos( WND* wndPtr, POINT pt )
{
    RECT rectParent;
    short x, y, xspacing, yspacing;

    GetClientRect( wndPtr->parent->hwndSelf, &rectParent );
    if ((pt.x >= rectParent.left) && (pt.x + GetSystemMetrics(SM_CXICON) < rectParent.right) &&
        (pt.y >= rectParent.top) && (pt.y + GetSystemMetrics(SM_CYICON) < rectParent.bottom))
        return pt;  /* The icon already has a suitable position */

    xspacing = GetSystemMetrics(SM_CXICONSPACING);
    yspacing = GetSystemMetrics(SM_CYICONSPACING);

    y = rectParent.bottom;
    for (;;)
    {
        x = rectParent.left;
        do
        {
              /* Check if another icon already occupies this spot */
            WND *childPtr = WIN_LockWndPtr(wndPtr->parent->child);
            while (childPtr)
            {
                if ((childPtr->dwStyle & WS_MINIMIZE) && (childPtr != wndPtr))
                {
                    if ((childPtr->rectWindow.left < x + xspacing) &&
                        (childPtr->rectWindow.right >= x) &&
                        (childPtr->rectWindow.top <= y) &&
                        (childPtr->rectWindow.bottom > y - yspacing))
                        break;  /* There's a window in there */
                }
                WIN_UpdateWndPtr(&childPtr,childPtr->next);
            }
            WIN_ReleaseWndPtr(childPtr);
            if (!childPtr) /* No window was found, so it's OK for us */
            {
                pt.x = x + (xspacing - GetSystemMetrics(SM_CXICON)) / 2;
                pt.y = y - (yspacing + GetSystemMetrics(SM_CYICON)) / 2;
                return pt;
            }
            x += xspacing;
        } while(x <= rectParent.right-xspacing);
        y -= yspacing;
    }
}





UINT WINPOS_MinMaximize( HWND hwnd, UINT cmd, LPRECT rect )
{
    WND *wndPtr = WIN_FindWndPtr( hwnd );
    UINT swpFlags = 0;
    POINT size;
    WINDOWPLACEMENT wpl;

    TRACE("0x%04x %u\n", hwnd, cmd );

    wpl.length = sizeof(wpl);
    GetWindowPlacement( hwnd, &wpl );

    size.x = wndPtr->rectWindow.left;
    size.y = wndPtr->rectWindow.top;

    if (!HOOK_CallHooksA(WH_CBT, HCBT_MINMAX, wndPtr->hwndSelf, cmd))
    {
        if( wndPtr->dwStyle & WS_MINIMIZE )
        {
            if( !SendMessageA( wndPtr->hwndSelf, WM_QUERYOPEN, 0, 0L ) )
            {
                swpFlags = SWP_NOSIZE | SWP_NOMOVE;
                goto done;
            }
            swpFlags |= SWP_NOCOPYBITS;
        }
        switch( cmd )
        {
        case SW_MINIMIZE:
            if( wndPtr->dwStyle & WS_MAXIMIZE)
            {
                wndPtr->flags |= WIN_RESTORE_MAX;
                wndPtr->dwStyle &= ~WS_MAXIMIZE;
            }
            else
                wndPtr->flags &= ~WIN_RESTORE_MAX;
            wndPtr->dwStyle |= WS_MINIMIZE;

            X11DRV_set_iconic_state( wndPtr );

            wpl.ptMinPosition = WINPOS_FindIconPos( wndPtr, wpl.ptMinPosition );

            SetRect( rect, wpl.ptMinPosition.x, wpl.ptMinPosition.y,
                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON) );
            swpFlags |= SWP_NOCOPYBITS;
            break;

        case SW_MAXIMIZE:
            WINPOS_GetMinMaxInfo( wndPtr, &size, &wpl.ptMaxPosition, NULL, NULL );

            if( wndPtr->dwStyle & WS_MINIMIZE )
            {
                wndPtr->dwStyle &= ~WS_MINIMIZE;
                WINPOS_ShowIconTitle( wndPtr, FALSE );
                X11DRV_set_iconic_state( wndPtr );
            }
            wndPtr->dwStyle |= WS_MAXIMIZE;

            SetRect( rect, wpl.ptMaxPosition.x, wpl.ptMaxPosition.y, size.x, size.y );
            break;

        case SW_RESTORE:
            if( wndPtr->dwStyle & WS_MINIMIZE )
            {
                wndPtr->dwStyle &= ~WS_MINIMIZE;
                WINPOS_ShowIconTitle( wndPtr, FALSE );
                X11DRV_set_iconic_state( wndPtr );

                if( wndPtr->flags & WIN_RESTORE_MAX)
                {
                    /* Restore to maximized position */
                    WINPOS_GetMinMaxInfo( wndPtr, &size, &wpl.ptMaxPosition, NULL, NULL);
                    wndPtr->dwStyle |= WS_MAXIMIZE;
                    SetRect( rect, wpl.ptMaxPosition.x, wpl.ptMaxPosition.y, size.x, size.y );
                    break;
                }
            }
            else
                if( !(wndPtr->dwStyle & WS_MAXIMIZE) )
                {
                    swpFlags = (UINT16)(-1);
                    goto done;
                }
                else wndPtr->dwStyle &= ~WS_MAXIMIZE;

            /* Restore to normal position */

            *rect = wpl.rcNormalPosition;
            rect->right -= rect->left;
            rect->bottom -= rect->top;

            break;
        }
    } else swpFlags |= SWP_NOSIZE | SWP_NOMOVE;

 done:
    WIN_ReleaseWndPtr( wndPtr );
    return swpFlags;
}


/***********************************************************************
 *              X11DRV_ShowWindow   (X11DRV.@)
 */
BOOL X11DRV_ShowWindow( HWND hwnd, INT cmd )
{
    WND* 	wndPtr = WIN_FindWndPtr( hwnd );
    BOOL 	wasVisible, showFlag;
    RECT 	newPos = {0, 0, 0, 0};
    UINT 	swp = 0;

    if (!wndPtr) return FALSE;

    TRACE("hwnd=%04x, cmd=%d\n", hwnd, cmd);

    wasVisible = (wndPtr->dwStyle & WS_VISIBLE) != 0;

    switch(cmd)
    {
        case SW_HIDE:
            if (!wasVisible) goto END;;
	    swp |= SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | 
		        SWP_NOACTIVATE | SWP_NOZORDER;
	    break;

	case SW_SHOWMINNOACTIVE:
            swp |= SWP_NOACTIVATE | SWP_NOZORDER;
            /* fall through */
	case SW_SHOWMINIMIZED:
            swp |= SWP_SHOWWINDOW;
            /* fall through */
	case SW_MINIMIZE:
            swp |= SWP_FRAMECHANGED;
            if( !(wndPtr->dwStyle & WS_MINIMIZE) )
		 swp |= WINPOS_MinMaximize( hwnd, SW_MINIMIZE, &newPos );
            else swp |= SWP_NOSIZE | SWP_NOMOVE;
	    break;

	case SW_SHOWMAXIMIZED: /* same as SW_MAXIMIZE */
            swp |= SWP_SHOWWINDOW | SWP_FRAMECHANGED;
            if( !(wndPtr->dwStyle & WS_MAXIMIZE) )
		 swp |= WINPOS_MinMaximize( hwnd, SW_MAXIMIZE, &newPos );
            else swp |= SWP_NOSIZE | SWP_NOMOVE;
            break;

	case SW_SHOWNA:
            swp |= SWP_NOACTIVATE | SWP_NOZORDER;
            /* fall through */
	case SW_SHOW:
	    swp |= SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;

	    /*
	     * ShowWindow has a little peculiar behavior that if the
	     * window is already the topmost window, it will not
	     * activate it.
	     */
	    if (GetTopWindow((HWND)0)==hwnd && (wasVisible || GetActiveWindow() == hwnd))
	      swp |= SWP_NOACTIVATE;

	    break;

	case SW_SHOWNOACTIVATE:
            swp |= SWP_NOZORDER;
            if (GetActiveWindow()) swp |= SWP_NOACTIVATE;
            /* fall through */
	case SW_SHOWNORMAL:  /* same as SW_NORMAL: */
	case SW_SHOWDEFAULT: /* FIXME: should have its own handler */
	case SW_RESTORE:
	    swp |= SWP_SHOWWINDOW | SWP_FRAMECHANGED;

            if( wndPtr->dwStyle & (WS_MINIMIZE | WS_MAXIMIZE) )
		 swp |= WINPOS_MinMaximize( hwnd, SW_RESTORE, &newPos );
            else swp |= SWP_NOSIZE | SWP_NOMOVE;
	    break;
    }

    showFlag = (cmd != SW_HIDE);
    if (showFlag != wasVisible)
    {
        SendMessageA( hwnd, WM_SHOWWINDOW, showFlag, 0 );
        if (!IsWindow( hwnd )) goto END;
    }

    /* We can't activate a child window */
    if ((wndPtr->dwStyle & WS_CHILD) &&
        !(wndPtr->dwExStyle & WS_EX_MDICHILD))
        swp |= SWP_NOACTIVATE | SWP_NOZORDER;

    SetWindowPos( hwnd, HWND_TOP, newPos.left, newPos.top,
                  newPos.right, newPos.bottom, LOWORD(swp) );
    if (cmd == SW_HIDE)
    {
        /* FIXME: This will cause the window to be activated irrespective
         * of whether it is owned by the same thread. Has to be done
         * asynchronously.
         */

        if (hwnd == GetActiveWindow())
            WINPOS_ActivateOtherWindow(wndPtr);

        /* Revert focus to parent */
        if (hwnd == GetFocus() || IsChild(hwnd, GetFocus()))
            SetFocus( GetParent(hwnd) );
    }
    if (!IsWindow( hwnd )) goto END;
    else if( wndPtr->dwStyle & WS_MINIMIZE ) WINPOS_ShowIconTitle( wndPtr, TRUE );

    if (wndPtr->flags & WIN_NEED_SIZE)
    {
        /* should happen only in CreateWindowEx() */
	int wParam = SIZE_RESTORED;

	wndPtr->flags &= ~WIN_NEED_SIZE;
	if (wndPtr->dwStyle & WS_MAXIMIZE) wParam = SIZE_MAXIMIZED;
	else if (wndPtr->dwStyle & WS_MINIMIZE) wParam = SIZE_MINIMIZED;
	SendMessageA( hwnd, WM_SIZE, wParam,
		     MAKELONG(wndPtr->rectClient.right-wndPtr->rectClient.left,
			    wndPtr->rectClient.bottom-wndPtr->rectClient.top));
	SendMessageA( hwnd, WM_MOVE, 0,
		   MAKELONG(wndPtr->rectClient.left, wndPtr->rectClient.top) );
    }

END:
    WIN_ReleaseWndPtr(wndPtr);
    return wasVisible;
}


/**********************************************************************
 *		X11DRV_MapNotify
 */
void X11DRV_MapNotify( HWND hwnd, XMapEvent *event )
{
    HWND hwndFocus = GetFocus();
    WND *win;

    if (!(win = WIN_FindWndPtr( hwnd ))) return;

    if ((win->dwStyle & WS_VISIBLE) &&
        (win->dwStyle & WS_MINIMIZE) &&
        (win->dwExStyle & WS_EX_MANAGED))
    {
        int x, y;
        unsigned int width, height, border, depth;
        Window root, top;
        RECT rect;

        DCE_InvalidateDCE( win, &win->rectWindow );
        win->dwStyle &= ~WS_MINIMIZE;
        win->dwStyle |= WS_VISIBLE;
        WIN_InternalShowOwnedPopups( hwnd, TRUE, TRUE );

        if (win->flags & WIN_RESTORE_MAX)
            win->dwStyle |= WS_MAXIMIZE;
        else
            win->dwStyle &= ~WS_MAXIMIZE;

        /* FIXME: hack */
        wine_tsx11_lock();
        XGetGeometry( event->display, get_whole_window(win), &root, &x, &y, &width, &height,
                        &border, &depth );
        XTranslateCoordinates( event->display, get_whole_window(win), root, 0, 0, &x, &y, &top );
        wine_tsx11_unlock();
        rect.left   = x;
        rect.top    = y;
        rect.right  = x + width;
        rect.bottom = y + height;
        X11DRV_X_to_window_rect( win, &rect );

        SendMessageA( hwnd, WM_SHOWWINDOW, SW_RESTORE, 0 );
        SetWindowPos( hwnd, 0, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
                      SWP_NOZORDER | SWP_WINE_NOHOSTMOVE );
    }
    if (hwndFocus && IsChild( hwnd, hwndFocus )) X11DRV_SetFocus(hwndFocus);  /* FIXME */
    WIN_ReleaseWndPtr( win );
}


/**********************************************************************
 *              X11DRV_UnmapNotify
 */
void X11DRV_UnmapNotify( HWND hwnd, XUnmapEvent *event )
{
    WND *win;

    if (!(win = WIN_FindWndPtr( hwnd ))) return;

    if ((win->dwStyle & WS_VISIBLE) && (win->dwExStyle & WS_EX_MANAGED))
    {
        EndMenu();
        SendMessageA( hwnd, WM_SHOWWINDOW, SW_MINIMIZE, 0 );

        win->flags &= ~WIN_RESTORE_MAX;
        win->dwStyle |= WS_MINIMIZE;

        if (win->dwStyle & WS_MAXIMIZE)
        {
            win->flags |= WIN_RESTORE_MAX;
            win->dwStyle &= ~WS_MAXIMIZE;
        }

        SetWindowPos( hwnd, 0, 0, 0, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                      SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_WINE_NOHOSTMOVE );

        WIN_InternalShowOwnedPopups( hwnd, FALSE, TRUE );
    }
    WIN_ReleaseWndPtr( win );
}


/***********************************************************************
 *           query_zorder
 *
 * Synchronize internal z-order with the window manager's.
 */
static BOOL __check_query_condition( WND** pWndA, WND** pWndB )
{
    /* return TRUE if we have at least two managed windows */

    for( *pWndB = NULL; *pWndA; *pWndA = (*pWndA)->next )
        if( ((*pWndA)->dwExStyle & WS_EX_MANAGED) &&
            ((*pWndA)->dwStyle & WS_VISIBLE )) break;
    if( *pWndA )
        for( *pWndB = (*pWndA)->next; *pWndB; *pWndB = (*pWndB)->next )
            if( ((*pWndB)->dwExStyle & WS_EX_MANAGED) &&
                ((*pWndB)->dwStyle & WS_VISIBLE )) break;
    return ((*pWndB) != NULL);
}

static Window __get_common_ancestor( Display *display, Window A, Window B,
                                     Window** children, unsigned* total )
{
    /* find the real root window */

    Window      root, *childrenB;
    unsigned    totalB;

    while( A != B && A && B )
    {
      TSXQueryTree( display, A, &root, &A, children, total );
      TSXQueryTree( display, B, &root, &B, &childrenB, &totalB );
      if( childrenB ) TSXFree( childrenB );
      if( *children ) TSXFree( *children ), *children = NULL;
    }

    if( A && B )
    {
        TSXQueryTree( display, A, &root, &B, children, total );
        return A;
    }
    return 0 ;
}

static Window __get_top_decoration( Display *display, Window w, Window ancestor )
{
    Window*     children, root, prev = w, parent = w;
    unsigned    total;

    do
    {
        w = parent;
        TSXQueryTree( display, w, &root, &parent, &children, &total );
        if( children ) TSXFree( children );
    } while( parent && parent != ancestor );
    TRACE("\t%08x -> %08x\n", (unsigned)prev, (unsigned)w );
    return ( parent ) ? w : 0 ;
}

static unsigned __td_lookup( Window w, Window* list, unsigned max )
{
    unsigned    i;
    for( i = max - 1; i >= 0; i-- ) if( list[i] == w ) break;
    return i;
}

static HWND query_zorder( Display *display, HWND hWndCheck)
{
    HWND      hwndInsertAfter = HWND_TOP;
    WND      *pWndCheck = WIN_FindWndPtr(hWndCheck);
    WND      *pDesktop = WIN_GetDesktop();
    WND      *pWnd, *pWndZ = WIN_LockWndPtr(pDesktop->child);
    Window      w, parent, *children = NULL;
    unsigned    total, check, pos, best;

    if( !__check_query_condition(&pWndZ, &pWnd) )
    {
        WIN_ReleaseWndPtr(pWndCheck);
        WIN_ReleaseWndPtr(pDesktop->child);
        WIN_ReleaseDesktop();
        return hwndInsertAfter;
    }
    WIN_LockWndPtr(pWndZ);
    WIN_LockWndPtr(pWnd);
    WIN_ReleaseWndPtr(pDesktop->child);
    WIN_ReleaseDesktop();

    parent = __get_common_ancestor( display, get_whole_window(pWndZ),
                                    get_whole_window(pWnd), &children, &total );
    if( parent && children )
    {
        /* w is the ancestor if pWndCheck that is a direct descendant of 'parent' */

        w = __get_top_decoration( display, get_whole_window(pWndCheck), parent );

        if( w != children[total-1] ) /* check if at the top */
        {
            /* X child at index 0 is at the bottom, at index total-1 is at the top */
            check = __td_lookup( w, children, total );
            best = total;

            for( WIN_UpdateWndPtr(&pWnd,pWndZ); pWnd;WIN_UpdateWndPtr(&pWnd,pWnd->next))
            {
                /* go through all windows in Wine z-order... */

                if( pWnd != pWndCheck )
                {
                    if( !(pWnd->dwExStyle & WS_EX_MANAGED) ||
                        !(w = __get_top_decoration( display, get_whole_window(pWnd), parent )) )
                        continue;
                    pos = __td_lookup( w, children, total );
                    if( pos < best && pos > check )
                    {
                        /* find a nearest Wine window precedes 
                         * pWndCheck in the real z-order... */
                        best = pos;
                        hwndInsertAfter = pWnd->hwndSelf;
                    }
                    if( best - check == 1 ) break;
                }
            }
        }
    }
    if( children ) TSXFree( children );
    WIN_ReleaseWndPtr(pWnd);
    WIN_ReleaseWndPtr(pWndZ);
    WIN_ReleaseWndPtr(pWndCheck);
    return hwndInsertAfter;
}


/***********************************************************************
 *		X11DRV_ConfigureNotify
 */
void X11DRV_ConfigureNotify( HWND hwnd, XConfigureEvent *event )
{
    HWND oldInsertAfter;
    struct x11drv_win_data *data;
    WND *win;
    RECT rect;
    WINDOWPOS winpos;
    int x = event->x, y = event->y;

    if (!(win = WIN_FindWndPtr( hwnd ))) return;
    data = win->pDriverData;

    /* Get geometry */

    if (!event->send_event)  /* normal event, need to map coordinates to the root */
    {
        Window child;
        wine_tsx11_lock();
        XTranslateCoordinates( event->display, data->whole_window, root_window,
                               0, 0, &x, &y, &child );
        wine_tsx11_unlock();
    }
    rect.left   = x;
    rect.top    = y;
    rect.right  = x + event->width;
    rect.bottom = y + event->height;
    TRACE( "win %x new X rect %d,%d,%dx%d (event %d,%d,%dx%d)\n",
           hwnd, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
           event->x, event->y, event->width, event->height );
    X11DRV_X_to_window_rect( win, &rect );
    WIN_ReleaseWndPtr( win );

    winpos.hwnd  = hwnd;
    winpos.x     = rect.left;
    winpos.y     = rect.top;
    winpos.cx    = rect.right - rect.left;
    winpos.cy    = rect.bottom - rect.top;
    winpos.flags = SWP_NOACTIVATE;

    /* Get Z-order (FIXME) */

    winpos.hwndInsertAfter = query_zorder( event->display, hwnd );

    /* needs to find the first Visible Window above the current one */
    oldInsertAfter = hwnd;
    for (;;)
    {
        oldInsertAfter = GetWindow( oldInsertAfter, GW_HWNDPREV );
        if (!oldInsertAfter)
        {
            oldInsertAfter = HWND_TOP;
            break;
        }
        if (GetWindowLongA( oldInsertAfter, GWL_STYLE ) & WS_VISIBLE) break;
    }

    /* Compare what has changed */

    GetWindowRect( hwnd, &rect );
    if (rect.left == winpos.x && rect.top == winpos.y) winpos.flags |= SWP_NOMOVE;
    else
        TRACE( "%04x moving from (%d,%d) to (%d,%d)\n",
               hwnd, rect.left, rect.top, winpos.x, winpos.y );

    if (rect.right - rect.left == winpos.cx &&
        rect.bottom - rect.top == winpos.cy) winpos.flags |= SWP_NOSIZE;
    else
        TRACE( "%04x resizing from (%dx%d) to (%dx%d)\n",
               hwnd, rect.right - rect.left, rect.bottom - rect.top,
               winpos.cx, winpos.cy );

    if (winpos.hwndInsertAfter == oldInsertAfter) winpos.flags |= SWP_NOZORDER;
    else
        TRACE( "%04x restacking from after %04x to after %04x\n",
               hwnd, oldInsertAfter, winpos.hwndInsertAfter );

    /* if nothing changed, don't do anything */
    if (winpos.flags == (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) return;

    SetWindowPos( hwnd, winpos.hwndInsertAfter, winpos.x, winpos.y,
                  winpos.cx, winpos.cy, winpos.flags | SWP_WINE_NOHOSTMOVE );
}


/***********************************************************************
 *		SetWindowRgn  (X11DRV.@)
 *
 * Assign specified region to window (for non-rectangular windows)
 */
BOOL X11DRV_SetWindowRgn( HWND hwnd, HRGN hrgn, BOOL redraw )
{
    RECT rect;
    WND *wndPtr = WIN_FindWndPtr(hwnd);
    int ret = FALSE;

    if (!wndPtr) return FALSE;

    if (wndPtr->hrgnWnd == hrgn)
    {
        ret = TRUE;
        goto done;
    }

    if (hrgn) /* verify that region really exists */
    {
        if (GetRgnBox( hrgn, &rect ) == ERROR) goto done;
    }

    if (wndPtr->hrgnWnd)
    {
        /* delete previous region */
        DeleteObject(wndPtr->hrgnWnd);
        wndPtr->hrgnWnd = 0;
    }
    wndPtr->hrgnWnd = hrgn;

    /* Size the window to the rectangle of the new region (if it isn't NULL) */
    if (hrgn) SetWindowPos( hwnd, 0, rect.left, rect.top,
                            rect.right  - rect.left, rect.bottom - rect.top,
                            SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOACTIVATE |
                            SWP_NOZORDER | (redraw ? 0 : SWP_NOREDRAW) );
#ifdef HAVE_LIBXSHAPE
    {
        Display *display = thread_display();
        X11DRV_WND_DATA *data = wndPtr->pDriverData;

        if (data->whole_window)
        {
            if (!hrgn)
            {
                TSXShapeCombineMask( display, data->whole_window,
                                     ShapeBounding, 0, 0, None, ShapeSet );
            }
            else
            {
                XRectangle *aXRect;
                int x_offset, y_offset;
                DWORD size;
                DWORD dwBufferSize = GetRegionData(hrgn, 0, NULL);
                PRGNDATA pRegionData = HeapAlloc(GetProcessHeap(), 0, dwBufferSize);
                if (!pRegionData) goto done;

                GetRegionData(hrgn, dwBufferSize, pRegionData);
                size = pRegionData->rdh.nCount;
                x_offset = wndPtr->rectWindow.left - data->whole_rect.left;
                y_offset = wndPtr->rectWindow.top - data->whole_rect.top;
                /* convert region's "Windows rectangles" to XRectangles */
                aXRect = HeapAlloc(GetProcessHeap(), 0, size * sizeof(*aXRect) );
                if (aXRect)
                {
                    XRectangle* pCurrRect = aXRect;
                    RECT *pRect = (RECT*) pRegionData->Buffer;
                    for (; pRect < ((RECT*) pRegionData->Buffer) + size ; ++pRect, ++pCurrRect)
                    {
                        pCurrRect->x      = pRect->left + x_offset;
                        pCurrRect->y      = pRect->top + y_offset;
                        pCurrRect->height = pRect->bottom - pRect->top;
                        pCurrRect->width  = pRect->right  - pRect->left;

                        TRACE("Rectangle %04d of %04ld data: X=%04d, Y=%04d, Height=%04d, Width=%04d.\n",
                              pRect - (RECT*) pRegionData->Buffer,
                              size,
                              pCurrRect->x,
                              pCurrRect->y,
                              pCurrRect->height,
                              pCurrRect->width);
                    }

                    /* shape = non-rectangular windows (X11/extensions) */
                    TSXShapeCombineRectangles( display, data->whole_window, ShapeBounding,
                                               0, 0, aXRect,
                                               pCurrRect - aXRect, ShapeSet, YXBanded );
                    HeapFree(GetProcessHeap(), 0, aXRect );
                }
                HeapFree(GetProcessHeap(), 0, pRegionData);
            }
        }
    }
#endif  /* HAVE_LIBXSHAPE */

    ret = TRUE;

 done:
    WIN_ReleaseWndPtr(wndPtr);
    return ret;
}


/***********************************************************************
 *           draw_moving_frame
 *
 * Draw the frame used when moving or resizing window.
 *
 * FIXME:  This causes problems in Win95 mode.  (why?)
 */
static void draw_moving_frame( HDC hdc, RECT *rect, BOOL thickframe )
{
    if (thickframe)
    {
        const int width = GetSystemMetrics(SM_CXFRAME);
        const int height = GetSystemMetrics(SM_CYFRAME);

        HBRUSH hbrush = SelectObject( hdc, GetStockObject( GRAY_BRUSH ) );
        PatBlt( hdc, rect->left, rect->top,
                rect->right - rect->left - width, height, PATINVERT );
        PatBlt( hdc, rect->left, rect->top + height, width,
                rect->bottom - rect->top - height, PATINVERT );
        PatBlt( hdc, rect->left + width, rect->bottom - 1,
                rect->right - rect->left - width, -height, PATINVERT );
        PatBlt( hdc, rect->right - 1, rect->top, -width,
                rect->bottom - rect->top - height, PATINVERT );
        SelectObject( hdc, hbrush );
    }
    else DrawFocusRect( hdc, rect );
}


/***********************************************************************
 *           start_size_move
 *
 * Initialisation of a move or resize, when initiatied from a menu choice.
 * Return hit test code for caption or sizing border.
 */
static LONG start_size_move( WND* wndPtr, WPARAM wParam, POINT *capturePoint )
{
    LONG hittest = 0;
    POINT pt;
    MSG msg;
    RECT rectWindow;

    GetWindowRect(wndPtr->hwndSelf,&rectWindow);

    if ((wParam & 0xfff0) == SC_MOVE)
    {
        /* Move pointer at the center of the caption */
        RECT rect;
        NC_GetInsideRect( wndPtr->hwndSelf, &rect );
        if (wndPtr->dwStyle & WS_SYSMENU)
            rect.left += GetSystemMetrics(SM_CXSIZE) + 1;
        if (wndPtr->dwStyle & WS_MINIMIZEBOX)
            rect.right -= GetSystemMetrics(SM_CXSIZE) + 1;
        if (wndPtr->dwStyle & WS_MAXIMIZEBOX)
            rect.right -= GetSystemMetrics(SM_CXSIZE) + 1;
        pt.x = rectWindow.left + (rect.right - rect.left) / 2;
        pt.y = rectWindow.top + rect.top + GetSystemMetrics(SM_CYSIZE)/2;
        hittest = HTCAPTION;
        *capturePoint = pt;
    }
    else  /* SC_SIZE */
    {
        while(!hittest)
        {
            MSG_InternalGetMessage( &msg, 0, 0, WM_KEYFIRST, WM_MOUSELAST,
                                    MSGF_SIZE, PM_REMOVE, FALSE, NULL );
            switch(msg.message)
            {
            case WM_MOUSEMOVE:
                hittest = NC_HandleNCHitTest( wndPtr->hwndSelf, msg.pt );
                if ((hittest < HTLEFT) || (hittest > HTBOTTOMRIGHT))
                    hittest = 0;
                break;

            case WM_LBUTTONUP:
                return 0;

            case WM_KEYDOWN:
                switch(msg.wParam)
                {
                case VK_UP:
                    hittest = HTTOP;
                    pt.x =(rectWindow.left+rectWindow.right)/2;
                    pt.y = rectWindow.top + GetSystemMetrics(SM_CYFRAME) / 2;
                    break;
                case VK_DOWN:
                    hittest = HTBOTTOM;
                    pt.x =(rectWindow.left+rectWindow.right)/2;
                    pt.y = rectWindow.bottom - GetSystemMetrics(SM_CYFRAME) / 2;
                    break;
                case VK_LEFT:
                    hittest = HTLEFT;
                    pt.x = rectWindow.left + GetSystemMetrics(SM_CXFRAME) / 2;
                    pt.y =(rectWindow.top+rectWindow.bottom)/2;
                    break;
                case VK_RIGHT:
                    hittest = HTRIGHT;
                    pt.x = rectWindow.right - GetSystemMetrics(SM_CXFRAME) / 2;
                    pt.y =(rectWindow.top+rectWindow.bottom)/2;
                    break;
                case VK_RETURN:
                case VK_ESCAPE: return 0;
                }
            }
        }
        *capturePoint = pt;
    }
    SetCursorPos( pt.x, pt.y );
    NC_HandleSetCursor( wndPtr->hwndSelf,
                        wndPtr->hwndSelf, MAKELONG( hittest, WM_MOUSEMOVE ));
    return hittest;
}


/***********************************************************************
 *           X11DRV_SysCommandSizeMove   (X11DRV.@)
 *
 * Perform SC_MOVE and SC_SIZE commands.
 */
void X11DRV_SysCommandSizeMove( HWND hwnd, WPARAM wParam )
{
    MSG msg;
    RECT sizingRect, mouseRect, origRect;
    HDC hdc;
    LONG hittest = (LONG)(wParam & 0x0f);
    HCURSOR16 hDragCursor = 0, hOldCursor = 0;
    POINT minTrack, maxTrack;
    POINT capturePoint, pt;
    WND *     wndPtr = WIN_FindWndPtr( hwnd );
    BOOL    thickframe = HAS_THICKFRAME( wndPtr->dwStyle, wndPtr->dwExStyle );
    BOOL    iconic = wndPtr->dwStyle & WS_MINIMIZE;
    BOOL    moved = FALSE;
    DWORD     dwPoint = GetMessagePos ();
    BOOL DragFullWindows = FALSE;
    BOOL grab;
    int iWndsLocks;
    Display *old_gdi_display = NULL;
    Display *display = thread_display();

    SystemParametersInfoA(SPI_GETDRAGFULLWINDOWS, 0, &DragFullWindows, 0);

    pt.x = SLOWORD(dwPoint);
    pt.y = SHIWORD(dwPoint);
    capturePoint = pt;

    if (IsZoomed(hwnd) || !IsWindowVisible(hwnd) ||
        (wndPtr->dwExStyle & WS_EX_MANAGED)) goto END;

    if ((wParam & 0xfff0) == SC_MOVE)
    {
        if (!hittest) hittest = start_size_move( wndPtr, wParam, &capturePoint );
        if (!hittest) goto END;
    }
    else  /* SC_SIZE */
    {
        if (!thickframe) goto END;
        if ( hittest && hittest != HTSYSMENU ) hittest += 2;
        else
        {
            SetCapture(hwnd);
            hittest = start_size_move( wndPtr, wParam, &capturePoint );
            if (!hittest)
            {
                ReleaseCapture();
                goto END;
            }
        }
    }

      /* Get min/max info */

    WINPOS_GetMinMaxInfo( wndPtr, NULL, NULL, &minTrack, &maxTrack );
    sizingRect = wndPtr->rectWindow;
    origRect = sizingRect;
    if (wndPtr->dwStyle & WS_CHILD)
        GetClientRect( wndPtr->parent->hwndSelf, &mouseRect );
    else
        SetRect(&mouseRect, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (ON_LEFT_BORDER(hittest))
    {
        mouseRect.left  = max( mouseRect.left, sizingRect.right-maxTrack.x );
        mouseRect.right = min( mouseRect.right, sizingRect.right-minTrack.x );
    }
    else if (ON_RIGHT_BORDER(hittest))
    {
        mouseRect.left  = max( mouseRect.left, sizingRect.left+minTrack.x );
        mouseRect.right = min( mouseRect.right, sizingRect.left+maxTrack.x );
    }
    if (ON_TOP_BORDER(hittest))
    {
        mouseRect.top    = max( mouseRect.top, sizingRect.bottom-maxTrack.y );
        mouseRect.bottom = min( mouseRect.bottom,sizingRect.bottom-minTrack.y);
    }
    else if (ON_BOTTOM_BORDER(hittest))
    {
        mouseRect.top    = max( mouseRect.top, sizingRect.top+minTrack.y );
        mouseRect.bottom = min( mouseRect.bottom, sizingRect.top+maxTrack.y );
    }
    if (wndPtr->dwStyle & WS_CHILD)
    {
        MapWindowPoints( wndPtr->parent->hwndSelf, 0, (LPPOINT)&mouseRect, 2 );
    }

    /* Retrieve a default cache DC (without using the window style) */
    hdc = GetDCEx( wndPtr->parent->hwndSelf, 0, DCX_CACHE );

    if( iconic ) /* create a cursor for dragging */
    {
        HICON hIcon = GetClassLongA( hwnd, GCL_HICON);
        if(!hIcon) hIcon = (HICON)SendMessageA( hwnd, WM_QUERYDRAGICON, 0, 0L);
        if( hIcon ) hDragCursor =  CURSORICON_IconToCursor( hIcon, TRUE );
        if( !hDragCursor ) iconic = FALSE;
    }

    /* repaint the window before moving it around */
    RedrawWindow( hwnd, NULL, 0, RDW_UPDATENOW | RDW_ALLCHILDREN );

    SendMessageA( hwnd, WM_ENTERSIZEMOVE, 0, 0 );
    SetCapture( hwnd );

    /* grab the server only when moving top-level windows without desktop */
    grab = (!DragFullWindows && (root_window == DefaultRootWindow(gdi_display)) &&
            (wndPtr->parent->hwndSelf == GetDesktopWindow()));

    wine_tsx11_lock();
    if (grab)
    {
        XSync( gdi_display, False );
        XGrabServer( display );
        XSync( display, False );
        /* switch gdi display to the thread display, since the server is grabbed */
        old_gdi_display = gdi_display;
        gdi_display = display;
    }
    XGrabPointer( display, get_whole_window(wndPtr), False,
                  PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                  GrabModeAsync, GrabModeAsync, get_client_window(wndPtr->parent),
                  None, CurrentTime );
    wine_tsx11_unlock();

    while(1)
    {
        int dx = 0, dy = 0;

        MSG_InternalGetMessage( &msg, 0, 0, WM_KEYFIRST, WM_MOUSELAST,
                                MSGF_SIZE, PM_REMOVE, FALSE, NULL );

        /* Exit on button-up, Return, or Esc */
        if ((msg.message == WM_LBUTTONUP) ||
            ((msg.message == WM_KEYDOWN) &&
             ((msg.wParam == VK_RETURN) || (msg.wParam == VK_ESCAPE)))) break;

        if ((msg.message != WM_KEYDOWN) && (msg.message != WM_MOUSEMOVE))
            continue;  /* We are not interested in other messages */

        pt = msg.pt;

        if (msg.message == WM_KEYDOWN) switch(msg.wParam)
        {
        case VK_UP:    pt.y -= 8; break;
        case VK_DOWN:  pt.y += 8; break;
        case VK_LEFT:  pt.x -= 8; break;
        case VK_RIGHT: pt.x += 8; break;
        }

        pt.x = max( pt.x, mouseRect.left );
        pt.x = min( pt.x, mouseRect.right );
        pt.y = max( pt.y, mouseRect.top );
        pt.y = min( pt.y, mouseRect.bottom );

        dx = pt.x - capturePoint.x;
        dy = pt.y - capturePoint.y;

        if (dx || dy)
        {
            if( !moved )
            {
                moved = TRUE;

                if( iconic ) /* ok, no system popup tracking */
                {
                    hOldCursor = SetCursor(hDragCursor);
                    ShowCursor( TRUE );
                    WINPOS_ShowIconTitle( wndPtr, FALSE );
                }
                else if(!DragFullWindows)
                    draw_moving_frame( hdc, &sizingRect, thickframe );
            }

            if (msg.message == WM_KEYDOWN) SetCursorPos( pt.x, pt.y );
            else
            {
                RECT newRect = sizingRect;
                WPARAM wpSizingHit = 0;

                if (hittest == HTCAPTION) OffsetRect( &newRect, dx, dy );
                if (ON_LEFT_BORDER(hittest)) newRect.left += dx;
                else if (ON_RIGHT_BORDER(hittest)) newRect.right += dx;
                if (ON_TOP_BORDER(hittest)) newRect.top += dy;
                else if (ON_BOTTOM_BORDER(hittest)) newRect.bottom += dy;
                if(!iconic && !DragFullWindows) draw_moving_frame( hdc, &sizingRect, thickframe );
                capturePoint = pt;

                /* determine the hit location */
                if (hittest >= HTLEFT && hittest <= HTBOTTOMRIGHT)
                    wpSizingHit = WMSZ_LEFT + (hittest - HTLEFT);
                SendMessageA( hwnd, WM_SIZING, wpSizingHit, (LPARAM)&newRect );

                if (!iconic)
                {
                    if(!DragFullWindows)
                        draw_moving_frame( hdc, &newRect, thickframe );
                    else {
                        /* To avoid any deadlocks, all the locks on the windows
			   structures must be suspended before the SetWindowPos */
                        iWndsLocks = WIN_SuspendWndsLock();
                        SetWindowPos( hwnd, 0, newRect.left, newRect.top,
                                      newRect.right - newRect.left,
                                      newRect.bottom - newRect.top,
                                      ( hittest == HTCAPTION ) ? SWP_NOSIZE : 0 );
                        WIN_RestoreWndsLock(iWndsLocks);
                    }
                }
                sizingRect = newRect;
            }
        }
    }

    ReleaseCapture();
    if( iconic )
    {
        if( moved ) /* restore cursors, show icon title later on */
        {
            ShowCursor( FALSE );
            SetCursor( hOldCursor );
        }
        DestroyCursor( hDragCursor );
    }
    else if (moved && !DragFullWindows)
        draw_moving_frame( hdc, &sizingRect, thickframe );

    ReleaseDC( wndPtr->parent->hwndSelf, hdc );

    wine_tsx11_lock();
    XUngrabPointer( display, CurrentTime );
    if (grab)
    {
        XSync( display, False );
        XUngrabServer( display );
        XSync( display, False );
        gdi_display = old_gdi_display;
    }
    wine_tsx11_unlock();

    if (HOOK_CallHooksA( WH_CBT, HCBT_MOVESIZE, hwnd, (LPARAM)&sizingRect ))
        sizingRect = wndPtr->rectWindow;

    SendMessageA( hwnd, WM_EXITSIZEMOVE, 0, 0 );
    SendMessageA( hwnd, WM_SETVISIBLE, !IsIconic(hwnd), 0L);

    /* window moved or resized */
    if (moved)
    {
        /* To avoid any deadlocks, all the locks on the windows
	   structures must be suspended before the SetWindowPos */
        iWndsLocks = WIN_SuspendWndsLock();

        /* if the moving/resizing isn't canceled call SetWindowPos
         * with the new position or the new size of the window
         */
        if (!((msg.message == WM_KEYDOWN) && (msg.wParam == VK_ESCAPE)) )
        {
            /* NOTE: SWP_NOACTIVATE prevents document window activation in Word 6 */
            if(!DragFullWindows)
                SetWindowPos( hwnd, 0, sizingRect.left, sizingRect.top,
                              sizingRect.right - sizingRect.left,
                              sizingRect.bottom - sizingRect.top,
                              ( hittest == HTCAPTION ) ? SWP_NOSIZE : 0 );
        }
        else
        { /* restore previous size/position */
            if(DragFullWindows)
                SetWindowPos( hwnd, 0, origRect.left, origRect.top,
                              origRect.right - origRect.left,
                              origRect.bottom - origRect.top,
                              ( hittest == HTCAPTION ) ? SWP_NOSIZE : 0 );
        }

        WIN_RestoreWndsLock(iWndsLocks);
    }

    if (IsIconic(hwnd))
    {
        /* Single click brings up the system menu when iconized */

        if( !moved )
        {
            if( wndPtr->dwStyle & WS_SYSMENU )
                SendMessageA( hwnd, WM_SYSCOMMAND,
                              SC_MOUSEMENU + HTSYSMENU, MAKELONG(pt.x,pt.y));
        }
        else WINPOS_ShowIconTitle( wndPtr, TRUE );
    }

END:
    WIN_ReleaseWndPtr(wndPtr);
}
