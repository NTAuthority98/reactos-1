/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Windows
 * FILE:             subsystems/win32/win32k/ntuser/window.c
 * PROGRAMER:        Casper S. Hornstrup (chorns@users.sourceforge.net)
 */

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserWinpos);

/* GLOBALS *******************************************************************/

#define MINMAX_NOSWP  (0x00010000)

#define SWP_EX_NOCOPY 0x0001
#define SWP_EX_PAINTSELF 0x0002

#define  SWP_AGG_NOGEOMETRYCHANGE \
    (SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE)
#define  SWP_AGG_NOPOSCHANGE \
    (SWP_AGG_NOGEOMETRYCHANGE | SWP_NOZORDER)
#define  SWP_AGG_STATUSFLAGS \
    (SWP_AGG_NOPOSCHANGE | SWP_FRAMECHANGED | SWP_HIDEWINDOW | SWP_SHOWWINDOW)

#define EMPTYPOINT(pt) ((pt).x == -1 && (pt).y == -1)
#define PLACE_MIN               0x0001
#define PLACE_MAX               0x0002
#define PLACE_RECT              0x0004

VOID FASTCALL IntLinkWindow(PWND Wnd,PWND WndInsertAfter);

/* FUNCTIONS *****************************************************************/

BOOL FASTCALL
IntGetClientOrigin(PWND Window OPTIONAL, LPPOINT Point)
{
   Window = Window ? Window : UserGetDesktopWindow();
   if (Window == NULL)
   {
      Point->x = Point->y = 0;
      return FALSE;
   }
   Point->x = Window->rcClient.left;
   Point->y = Window->rcClient.top;

   return TRUE;
}

/*!
 * Internal function.
 * Returns client window rectangle relative to the upper-left corner of client area.
 *
 * \note Does not check the validity of the parameters
*/
VOID FASTCALL
IntGetClientRect(PWND Wnd, RECTL *Rect)
{
   ASSERT( Wnd );
   ASSERT( Rect );
   if (Wnd->style & WS_MINIMIZED)
   {
      Rect->left = Rect->top = 0;
      Rect->right = UserGetSystemMetrics(SM_CXMINIMIZED);
      Rect->bottom = UserGetSystemMetrics(SM_CYMINIMIZED);
      return;
   }
   if ( Wnd != UserGetDesktopWindow()) // Wnd->fnid != FNID_DESKTOP )
   {
      *Rect = Wnd->rcClient;
      RECTL_vOffsetRect(Rect, -Wnd->rcClient.left, -Wnd->rcClient.top);
   }
   else
   {
      Rect->left = Rect->top = 0;
      Rect->right  = Wnd->rcClient.right;
      Rect->bottom = Wnd->rcClient.bottom;
   /* Do this until Init bug is fixed. This sets 640x480, see InitMetrics.
      Rect->right  = UserGetSystemMetrics(SM_CXSCREEN);
      Rect->bottom = UserGetSystemMetrics(SM_CYSCREEN);
   */
   }
}

INT FASTCALL
IntMapWindowPoints(PWND FromWnd, PWND ToWnd, LPPOINT lpPoints, UINT cPoints)
{
    BOOL mirror_from, mirror_to;
    POINT Delta;
    UINT i;
    int Change = 1;

    /* Note: Desktop Top and Left is always 0! */
    Delta.x = Delta.y = 0;
    mirror_from = mirror_to = FALSE;

    if (FromWnd && FromWnd != UserGetDesktopWindow()) // FromWnd->fnid != FNID_DESKTOP)
    {
       if (FromWnd->ExStyle & WS_EX_LAYOUTRTL)
       {
          mirror_from = TRUE;
          Change = -Change;
          Delta.x = -FromWnd->rcClient.right;
       }
       else
          Delta.x = FromWnd->rcClient.left;
       Delta.y = FromWnd->rcClient.top;
    }

    if (ToWnd && ToWnd != UserGetDesktopWindow()) // ToWnd->fnid != FNID_DESKTOP)
    {
       if (ToWnd->ExStyle & WS_EX_LAYOUTRTL)
       {
          mirror_to = TRUE;
          Change = -Change;
          Delta.x += Change * ToWnd->rcClient.right;
       }
       else
          Delta.x -= Change * ToWnd->rcClient.left;
       Delta.y -= ToWnd->rcClient.top;
    }

    if (mirror_from) Delta.x = -Delta.x;

    for (i = 0; i != cPoints; i++)
    {
        lpPoints[i].x += Delta.x;
        lpPoints[i].x *= Change;
        lpPoints[i].y += Delta.y;
    }

    if ((mirror_from || mirror_to) && cPoints == 2)  /* special case for rectangle */
    {
       int tmp = min(lpPoints[0].x, lpPoints[1].x);
       lpPoints[1].x = max(lpPoints[0].x, lpPoints[1].x);
       lpPoints[0].x = tmp;
    }

    return MAKELONG(LOWORD(Delta.x), LOWORD(Delta.y));
}

BOOL FASTCALL IsChildVisible(PWND pWnd)
{
    do
    {
        if ( (pWnd->style & (WS_POPUP|WS_CHILD)) != WS_CHILD ||
            !(pWnd = pWnd->spwndParent) )
           return TRUE;
    }
    while (pWnd->style & WS_VISIBLE);
    return FALSE;
}

PWND FASTCALL IntGetLastTopMostWindow(VOID)
{
    PWND pWnd;
    PDESKTOP rpdesk = gptiCurrent->rpdesk;

    if ( rpdesk &&
        (pWnd = rpdesk->pDeskInfo->spwnd->spwndChild) &&
         pWnd->ExStyle & WS_EX_TOPMOST)
    {
        for (;;)
        {
           if (!pWnd->spwndNext) break;
           if (!(pWnd->spwndNext->ExStyle & WS_EX_TOPMOST)) break;
           pWnd = pWnd->spwndNext;
        }
        return pWnd;
    }
    return NULL;
}

//
// This helps with bug 6751 forcing modal dialog active when another app is minimized or closed.
//
BOOL FASTCALL ActivateOtherWindowMin(PWND Wnd)
{
    BOOL ActivePrev, FindTopWnd;
    PWND pWndTopMost, pWndChild, pWndSetActive, pWndTemp, pWndDesk;
    USER_REFERENCE_ENTRY Ref;
    PTHREADINFO pti = gptiCurrent;

    //ERR("AOWM 1\n");
    ActivePrev = (pti->MessageQueue->spwndActivePrev != NULL);
    FindTopWnd = TRUE;

    if ((pWndTopMost = IntGetLastTopMostWindow()))
       pWndChild = pWndTopMost->spwndNext;
    else
       pWndChild = Wnd->spwndParent->spwndChild;

    for (;;)
    {
       if ( ActivePrev )
          pWndSetActive = pti->MessageQueue->spwndActivePrev;
       else
          pWndSetActive = pWndChild;

       pWndTemp = NULL;

       while(pWndSetActive)
       {
          if ( VerifyWnd(pWndSetActive) &&
              !(pWndSetActive->ExStyle & WS_EX_NOACTIVATE) &&
               (pWndSetActive->style & (WS_VISIBLE|WS_DISABLED)) == WS_VISIBLE &&
               (!(pWndSetActive->style & WS_ICONIC) /* FIXME MinMax pos? */ ) )
          {
             if (!(pWndSetActive->ExStyle & WS_EX_TOOLWINDOW) )
             {
                UserRefObjectCo(pWndSetActive, &Ref);
                co_IntSetForegroundWindow(pWndSetActive);
                UserDerefObjectCo(pWndSetActive);
                //ERR("AOWM 2 Exit Good\n");
                return TRUE;
             }
             if (!pWndTemp ) pWndTemp = pWndSetActive;
          }
          if ( ActivePrev )
          {
             ActivePrev = FALSE;
             pWndSetActive = pWndChild;
          }
          else
             pWndSetActive = pWndSetActive->spwndNext;
       }

       if ( !FindTopWnd ) break;
       FindTopWnd = FALSE;

       if ( pWndChild )
       {
          pWndChild = pWndChild->spwndParent->spwndChild;
          continue;
       }

       if (!(pWndDesk = IntGetThreadDesktopWindow(pti)))
       {
          pWndChild = NULL;
          continue;
       }
       pWndChild = pWndDesk->spwndChild;
    }

    if ((pWndSetActive = pWndTemp))
    {
       UserRefObjectCo(pWndSetActive, &Ref);
       co_IntSetForegroundWindow(pWndSetActive);
       UserDerefObjectCo(pWndSetActive);
       //ERR("AOWM 3 Exit Good\n");
       return TRUE;
    }
    //ERR("AOWM 4 Bad\n");
    return FALSE;
}

/*******************************************************************
 *         can_activate_window
 *
 * Check if we can activate the specified window.
 */
static
BOOL FASTCALL can_activate_window( PWND Wnd OPTIONAL)
{
    LONG style;

    if (!Wnd) return FALSE;

    style = Wnd->style;
    if (!(style & WS_VISIBLE)) return FALSE;
    if (style & WS_MINIMIZE) return FALSE;
    if ((style & (WS_POPUP|WS_CHILD)) == WS_CHILD) return FALSE;
    return TRUE;
    /* FIXME: This window could be disable  because the child that closed
              was a popup. */
    //return !(style & WS_DISABLED);
}


/*******************************************************************
 *         WinPosActivateOtherWindow
 *
 *  Activates window other than pWnd.
 */
VOID FASTCALL
co_WinPosActivateOtherWindow(PWND Wnd)
{
   PWND WndTo = NULL;
   HWND Fg, previous;
   USER_REFERENCE_ENTRY Ref;

   ASSERT_REFS_CO(Wnd);

   if (IntIsDesktopWindow(Wnd))
   {
      IntSetFocusMessageQueue(NULL);
      return;
   }

   /* If this is popup window, try to activate the owner first. */
   if ((Wnd->style & WS_POPUP) && (WndTo = Wnd->spwndOwner))
   {
      WndTo = UserGetAncestor( WndTo, GA_ROOT );
      if (can_activate_window(WndTo)) goto done;
   }

   /* Pick a next top-level window. */
   /* FIXME: Search for non-tooltip windows first. */
   WndTo = Wnd;
   for (;;)
   {
      if (!(WndTo = WndTo->spwndNext)) break;
      if (can_activate_window( WndTo )) break;
   }

done:

   if (WndTo) UserRefObjectCo(WndTo, &Ref);

   Fg = UserGetForegroundWindow();
   if ((!Fg || Wnd->head.h == Fg) && WndTo) // FIXME: Ok if WndTo is NULL?? No, rule #4.
   {
      /* FIXME: Wine can pass WndTo = NULL to co_IntSetForegroundWindow. Hmm... */
      if (co_IntSetForegroundWindow(WndTo))
      {
         UserDerefObjectCo(WndTo);
         return;
      }
   }

   if (!co_IntSetActiveWindow(WndTo,&previous,FALSE,TRUE,FALSE) ||  /* Ok for WndTo to be NULL here */
       !previous)
   {
      co_IntSetActiveWindow(0,NULL,FALSE,TRUE,FALSE);
   }
   if (WndTo) UserDerefObjectCo(WndTo);
}


UINT
FASTCALL
co_WinPosArrangeIconicWindows(PWND parent)
{
   RECTL rectParent;
   INT i, x, y, xspacing, yspacing, sx, sy;
   HWND *List = IntWinListChildren(parent);

   ASSERT_REFS_CO(parent);

   /* Check if we found any children */
   if(List == NULL)
   {
       return 0;
   }

   IntGetClientRect( parent, &rectParent );
   // FIXME: Support gspv.mm.iArrange.
   x = rectParent.left;
   y = rectParent.bottom;

   xspacing = (UserGetSystemMetrics(SM_CXMINSPACING)/2)+UserGetSystemMetrics(SM_CXBORDER);
   yspacing = (UserGetSystemMetrics(SM_CYMINSPACING)/2)+UserGetSystemMetrics(SM_CYBORDER);

   //ERR("X:%d Y:%d XS:%d YS:%d\n",x,y,xspacing,yspacing);

   for(i = 0; List[i]; i++)
   {
      PWND Child;

      if (!(Child = ValidateHwndNoErr(List[i])))
         continue;

      if((Child->style & WS_MINIMIZE) != 0 )
      {
         USER_REFERENCE_ENTRY Ref;
         UserRefObjectCo(Child, &Ref);

         sx = x + UserGetSystemMetrics(SM_CXBORDER);
         sy = y - yspacing - UserGetSystemMetrics(SM_CYBORDER);

         co_WinPosSetWindowPos( Child, 0, sx, sy, 0, 0,
                                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );

         Child->InternalPos.IconPos.x = sx;
         Child->InternalPos.IconPos.y = sy;
         Child->InternalPos.flags |= WPF_MININIT;
         Child->InternalPos.flags &= ~WPF_SETMINPOSITION;

         UserDerefObjectCo(Child);

         if (x <= (rectParent.right - UserGetSystemMetrics(SM_CXMINSPACING)))
            x += xspacing;
         else
         {
            x = rectParent.left;
            y -= yspacing;
         }
         //ERR("X:%d Y:%d\n",x,y);
      }
   }
   ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
   return yspacing;
}

static VOID FASTCALL
WinPosFindIconPos(PWND Window, POINT *Pos)
{
   RECT rectParent;
   PWND pwndChild, pwndParent;
   int x, y, xspacing, yspacing;

   pwndParent = Window->spwndParent;
   if (pwndParent == UserGetDesktopWindow())
   {
      //ERR("Parent is Desktop, Min off screen!\n");
      /* ReactOS doesn't support iconic minimize to desktop */
      Pos->x = Pos->y = -32000;
      Window->InternalPos.flags |= WPF_MININIT;
      Window->InternalPos.IconPos.x = Pos->x;
      Window->InternalPos.IconPos.y = Pos->y;
      return;
   }

   IntGetClientRect( pwndParent, &rectParent );
   // FIXME: Support gspv.mm.iArrange.
   x = rectParent.left;
   y = rectParent.bottom;

   xspacing = (UserGetSystemMetrics(SM_CXMINSPACING)/2)+UserGetSystemMetrics(SM_CXBORDER);
   yspacing = (UserGetSystemMetrics(SM_CYMINSPACING)/2)+UserGetSystemMetrics(SM_CYBORDER);

   //ERR("X:%d Y:%d XS:%d YS:%d\n",Pos->x,Pos->y,xspacing,yspacing);

   // Set to default position when minimized.
   Pos->x = x + UserGetSystemMetrics(SM_CXBORDER);
   Pos->y = y - yspacing - UserGetSystemMetrics(SM_CYBORDER);

   for (pwndChild = pwndParent->spwndChild; pwndChild; pwndChild = pwndChild->spwndNext)
   {
        if (pwndChild == Window) continue;

        if (pwndChild->style & WS_VISIBLE)
        {
            //ERR("Loop!\n");
            continue;
        }
        //ERR("Pos Child X %d Y %d!\n", pwndChild->InternalPos.IconPos.x, pwndChild->InternalPos.IconPos.y);
        if ( pwndChild->InternalPos.IconPos.x == Pos->x &&
             pwndChild->InternalPos.IconPos.y == Pos->y )
        {
           if (x <= rectParent.right - UserGetSystemMetrics(SM_CXMINSPACING))
              x += xspacing;
           else
           {
              x = rectParent.left;
              y -= yspacing;
           }
           Pos->x = x + UserGetSystemMetrics(SM_CXBORDER);
           Pos->y = y - yspacing - UserGetSystemMetrics(SM_CYBORDER);
        }
   }
   Window->InternalPos.IconPos.x = Pos->x;
   Window->InternalPos.IconPos.y = Pos->y;
   Window->InternalPos.flags |= WPF_MININIT;
   //ERR("Position is set! X:%d Y:%d\n",Pos->x,Pos->y);
   return;
}

VOID FASTCALL
WinPosInitInternalPos(PWND Wnd, RECTL *RestoreRect)
{
   POINT Size;
   RECTL Rect = *RestoreRect;

   if (Wnd->spwndParent != UserGetDesktopWindow())
   {
      RECTL_vOffsetRect(&Rect,
                        -Wnd->spwndParent->rcClient.left,
                        -Wnd->spwndParent->rcClient.top);
   }

   Size.x = Rect.left;
   Size.y = Rect.top;

   if (!Wnd->InternalPosInitialized)
   {
      // FIXME: Use check point Atom..
      Wnd->InternalPos.flags = 0;
      Wnd->InternalPos.MaxPos.x  = Wnd->InternalPos.MaxPos.y  = -1;
      Wnd->InternalPos.IconPos.x = Wnd->InternalPos.IconPos.y = -1;
      Wnd->InternalPos.NormalRect = Rect;
      Wnd->InternalPosInitialized = TRUE;
   }

   if (Wnd->style & WS_MINIMIZE)
   {
      Wnd->InternalPos.IconPos = Size;
      Wnd->InternalPos.flags |= WPF_MININIT;
   }
   else if (Wnd->style & WS_MAXIMIZE)
   {
      Wnd->InternalPos.flags |= WPF_MAXINIT;

      if ( Wnd->spwndParent == Wnd->head.rpdesk->pDeskInfo->spwnd )
      {
         if (Wnd->state & WNDS_MAXIMIZESTOMONITOR)
         {
            Wnd->InternalPos.flags &= ~WPF_MAXINIT;
            Wnd->InternalPos.MaxPos.x = Wnd->InternalPos.MaxPos.y = -1;
         }
         else
         {
            RECTL WorkArea;
            PMONITOR pmonitor = UserMonitorFromRect(&Rect, MONITOR_DEFAULTTOPRIMARY );
            // FIXME: support DPI aware, rcWorkDPI/Real etc..
            WorkArea = pmonitor->rcMonitor;

            if (Wnd->style & WS_MAXIMIZEBOX)
            {  // Support (Wnd->state & WNDS_HASCAPTION) || pmonitor->cFullScreen too.
               if ((Wnd->style & WS_CAPTION) == WS_CAPTION || !(Wnd->style & (WS_CHILD | WS_POPUP)))
               {
                  WorkArea = pmonitor->rcWork;
                  //ERR("rcWork\n");
               }
            }

            Wnd->InternalPos.MaxPos.x = Rect.left - WorkArea.left;
            Wnd->InternalPos.MaxPos.y = Rect.top  - WorkArea.top;

            /*ERR("WinPosIP 2 X %d = R.l %d - W.l %d | Y %d = R.t %d - W.t %d\n",
                                         Wnd->InternalPos.MaxPos.x,
                                         Rect.left, WorkArea.left,
                                         Wnd->InternalPos.MaxPos.y,
                                         Rect.top, WorkArea.top);*/
         }
      }
      else
         Wnd->InternalPos.MaxPos = Size;
   }
   else
   {
      Wnd->InternalPos.NormalRect = Rect;
   }
}

BOOL
FASTCALL
IntGetWindowPlacement(PWND Wnd, WINDOWPLACEMENT *lpwndpl)
{
   if (!Wnd) return FALSE;

   if(lpwndpl->length != sizeof(WINDOWPLACEMENT))
   {
      return FALSE;
   }

   lpwndpl->flags = 0;

   WinPosInitInternalPos(Wnd, &Wnd->rcWindow);

   lpwndpl->showCmd = SW_HIDE;

   if ( Wnd->style & WS_MINIMIZE )
      lpwndpl->showCmd = SW_SHOWMINIMIZED;
   else
      lpwndpl->showCmd = ( Wnd->style & WS_MAXIMIZE ) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL ;

   lpwndpl->rcNormalPosition = Wnd->InternalPos.NormalRect;

   if (Wnd->InternalPos.flags & WPF_MININIT) // Return if it was set!
   {
      lpwndpl->ptMinPosition.x = Wnd->InternalPos.IconPos.x;
      lpwndpl->ptMinPosition.y = Wnd->InternalPos.IconPos.y;
   }
   else
      lpwndpl->ptMinPosition.x = lpwndpl->ptMinPosition.y = -1;

   if ( Wnd->InternalPos.flags & WPF_MAXINIT && // Return if set and not maximized to monitor!
        !(Wnd->state & WNDS_MAXIMIZESTOMONITOR))
   {
      lpwndpl->ptMaxPosition.x = Wnd->InternalPos.MaxPos.x;
      lpwndpl->ptMaxPosition.y = Wnd->InternalPos.MaxPos.y;
   }
   else
      lpwndpl->ptMaxPosition.x = lpwndpl->ptMaxPosition.y = -1;

   if ( Wnd->spwndParent == Wnd->head.rpdesk->pDeskInfo->spwnd &&
       !(Wnd->ExStyle & WS_EX_TOOLWINDOW))
   {
      PMONITOR pmonitor = UserMonitorFromRect(&lpwndpl->rcNormalPosition, MONITOR_DEFAULTTOPRIMARY );

      // FIXME: support DPI aware, rcWorkDPI/Real etc..
      if (Wnd->InternalPos.flags & WPF_MININIT)
      {
         lpwndpl->ptMinPosition.x -= (pmonitor->rcWork.left - pmonitor->rcMonitor.left);
         lpwndpl->ptMinPosition.y -= (pmonitor->rcWork.top - pmonitor->rcMonitor.top);
      }
      RECTL_vOffsetRect(&lpwndpl->rcNormalPosition,
                         pmonitor->rcMonitor.left - pmonitor->rcWork.left,
                         pmonitor->rcMonitor.top  - pmonitor->rcWork.top);
   }

   if ( Wnd->InternalPos.flags & WPF_RESTORETOMAXIMIZED || Wnd->style & WS_MAXIMIZE )
      lpwndpl->flags |= WPF_RESTORETOMAXIMIZED;

   if ( ((Wnd->style & (WS_CHILD|WS_POPUP)) == WS_CHILD) && Wnd->InternalPos.flags & WPF_SETMINPOSITION)
      lpwndpl->flags |= WPF_SETMINPOSITION;

   return TRUE;
}

/* make sure the specified rect is visible on screen */
static void make_rect_onscreen( RECT *rect )
{
    PMONITOR pmonitor = UserMonitorFromRect( rect, MONITOR_DEFAULTTONEAREST ); // Wine uses this.

    //  FIXME: support DPI aware, rcWorkDPI/Real etc..
    if (!pmonitor) return;
    /* FIXME: map coordinates from rcWork to rcMonitor */
    if (rect->right <= pmonitor->rcWork.left)
    {
        rect->right += pmonitor->rcWork.left - rect->left;
        rect->left = pmonitor->rcWork.left;
    }
    else if (rect->left >= pmonitor->rcWork.right)
    {
        rect->left += pmonitor->rcWork.right - rect->right;
        rect->right = pmonitor->rcWork.right;
    }
    if (rect->bottom <= pmonitor->rcWork.top)
    {
        rect->bottom += pmonitor->rcWork.top - rect->top;
        rect->top = pmonitor->rcWork.top;
    }
    else if (rect->top >= pmonitor->rcWork.bottom)
    {
        rect->top += pmonitor->rcWork.bottom - rect->bottom;
        rect->bottom = pmonitor->rcWork.bottom;
    }
}

/* make sure the specified point is visible on screen */
static void make_point_onscreen( POINT *pt )
{
    RECT rect;

    RECTL_vSetRect( &rect, pt->x, pt->y, pt->x + 1, pt->y + 1 );
    make_rect_onscreen( &rect );
    pt->x = rect.left;
    pt->y = rect.top;
}

BOOL FASTCALL
IntSetWindowPlacement(PWND Wnd, WINDOWPLACEMENT *wpl, UINT Flags)
{
   BOOL sAsync;
   UINT SWP_Flags;

   if ( Flags & PLACE_MIN) make_point_onscreen( &wpl->ptMinPosition );
   if ( Flags & PLACE_MAX) make_point_onscreen( &wpl->ptMaxPosition );
   if ( Flags & PLACE_RECT) make_rect_onscreen( &wpl->rcNormalPosition );

   if (!Wnd || Wnd == Wnd->head.rpdesk->pDeskInfo->spwnd) return FALSE;

   if ( Flags & PLACE_MIN ) Wnd->InternalPos.IconPos = wpl->ptMinPosition;
   if ( Flags & PLACE_MAX ) Wnd->InternalPos.MaxPos = wpl->ptMaxPosition;
   if ( Flags & PLACE_RECT) Wnd->InternalPos.NormalRect = wpl->rcNormalPosition;

   SWP_Flags = SWP_NOZORDER | SWP_NOACTIVATE | ((wpl->flags & WPF_ASYNCWINDOWPLACEMENT) ? SWP_ASYNCWINDOWPOS : 0);

   if (Wnd->style & WS_MINIMIZE )
   {
      if (Flags & PLACE_MIN || Wnd->InternalPos.flags & WPF_SETMINPOSITION)
      {
         co_WinPosSetWindowPos(Wnd, HWND_TOP,
                               wpl->ptMinPosition.x, wpl->ptMinPosition.y, 0, 0,
                               SWP_NOSIZE | SWP_Flags);
         Wnd->InternalPos.flags |= WPF_MININIT;
      }
   }
   else if (Wnd->style & WS_MAXIMIZE )
   {
      if (Flags & PLACE_MAX)
      {
         co_WinPosSetWindowPos(Wnd, HWND_TOP,
                               wpl->ptMaxPosition.x, wpl->ptMaxPosition.y, 0, 0,
                               SWP_NOSIZE | SWP_Flags);
         Wnd->InternalPos.flags |= WPF_MAXINIT;
      }
   }
   else if (Flags & PLACE_RECT)
   {
      co_WinPosSetWindowPos(Wnd, HWND_TOP,
                            wpl->rcNormalPosition.left, wpl->rcNormalPosition.top,
                            wpl->rcNormalPosition.right - wpl->rcNormalPosition.left,
                            wpl->rcNormalPosition.bottom - wpl->rcNormalPosition.top,
                            SWP_Flags);
   }

   sAsync = (Wnd->head.pti->MessageQueue != gptiCurrent->MessageQueue && wpl->flags & WPF_ASYNCWINDOWPLACEMENT);

   if ( sAsync )
      co_IntSendMessageNoWait( UserHMGetHandle(Wnd), WM_ASYNC_SHOWWINDOW, wpl->showCmd, 0 );
   else
      co_WinPosShowWindow(Wnd, wpl->showCmd);

   if ( Wnd->style & WS_MINIMIZE && !sAsync )
   {
      if ( wpl->flags & WPF_SETMINPOSITION )
         Wnd->InternalPos.flags |= WPF_SETMINPOSITION;

      if ( wpl->flags & WPF_RESTORETOMAXIMIZED )
         Wnd->InternalPos.flags |= WPF_RESTORETOMAXIMIZED;
   }
   return TRUE;
}

UINT FASTCALL
co_WinPosMinMaximize(PWND Wnd, UINT ShowFlag, RECT* NewPos)
{
   POINT Size;
   WINDOWPLACEMENT wpl;
   LONG old_style;
   UINT SwpFlags = 0;

   ASSERT_REFS_CO(Wnd);

   wpl.length = sizeof(wpl);
   IntGetWindowPlacement( Wnd, &wpl );

   if (co_HOOK_CallHooks( WH_CBT, HCBT_MINMAX, (WPARAM)Wnd->head.h, ShowFlag))
   {
      ERR("WinPosMinMaximize WH_CBT Call Hook return!\n");
      return SWP_NOSIZE | SWP_NOMOVE;
   }
      if (Wnd->style & WS_MINIMIZE)
      {
         switch (ShowFlag)
         {
         case SW_SHOWMINNOACTIVE:
         case SW_SHOWMINIMIZED:
         case SW_FORCEMINIMIZE:
         case SW_MINIMIZE:
             return SWP_NOSIZE | SWP_NOMOVE;
         }
         if (!co_IntSendMessageNoWait(Wnd->head.h, WM_QUERYOPEN, 0, 0))
         {
            return(SWP_NOSIZE | SWP_NOMOVE);
         }
         SwpFlags |= SWP_NOCOPYBITS;
      }
      switch (ShowFlag)
      {
         case SW_SHOWMINNOACTIVE:
         case SW_SHOWMINIMIZED:
         case SW_FORCEMINIMIZE:
         case SW_MINIMIZE:
            {
               //ERR("MinMaximize Minimize\n");
               if (Wnd->style & WS_MAXIMIZE)
               {
                  Wnd->InternalPos.flags |= WPF_RESTORETOMAXIMIZED;
               }
               else
               {
                  Wnd->InternalPos.flags &= ~WPF_RESTORETOMAXIMIZED;
               }

               old_style = IntSetStyle( Wnd, WS_MINIMIZE, WS_MAXIMIZE );

               co_UserRedrawWindow(Wnd, NULL, 0, RDW_VALIDATE | RDW_NOERASE |
                                   RDW_NOINTERNALPAINT);

               if (!(Wnd->InternalPos.flags & WPF_SETMINPOSITION))
                  Wnd->InternalPos.flags &= ~WPF_MININIT;

               WinPosFindIconPos(Wnd, &wpl.ptMinPosition);

               if (!(old_style & WS_MINIMIZE)) SwpFlags |= SWP_STATECHANGED;

               /*ERR("Minimize: %d,%d %dx%d\n",
                      wpl.ptMinPosition.x, wpl.ptMinPosition.y, UserGetSystemMetrics(SM_CXMINIMIZED),
                                                   UserGetSystemMetrics(SM_CYMINIMIZED));
                */
               RECTL_vSetRect(NewPos, wpl.ptMinPosition.x, wpl.ptMinPosition.y,
//                             wpl.ptMinPosition.x + UserGetSystemMetrics(SM_CXMINIMIZED),
//                             wpl.ptMinPosition.y + UserGetSystemMetrics(SM_CYMINIMIZED));
                             UserGetSystemMetrics(SM_CXMINIMIZED),
                             UserGetSystemMetrics(SM_CYMINIMIZED));
               SwpFlags |= SWP_NOCOPYBITS;
               break;
            }

         case SW_MAXIMIZE:
            {
               //ERR("MinMaximize Maximize\n");
               if ((Wnd->style & WS_MAXIMIZE) && (Wnd->style & WS_VISIBLE))
               {
                  SwpFlags = SWP_NOSIZE | SWP_NOMOVE;
                  break;
               }
               co_WinPosGetMinMaxInfo(Wnd, &Size, &wpl.ptMaxPosition, NULL, NULL);

               /*ERR("Maximize: %d,%d %dx%d\n",
                      wpl.ptMaxPosition.x, wpl.ptMaxPosition.y, Size.x, Size.y);
                */
               old_style = IntSetStyle( Wnd, WS_MAXIMIZE, WS_MINIMIZE );

               if (!(old_style & WS_MAXIMIZE)) SwpFlags |= SWP_STATECHANGED;
               RECTL_vSetRect(NewPos, wpl.ptMaxPosition.x, wpl.ptMaxPosition.y,
//                              wpl.ptMaxPosition.x + Size.x, wpl.ptMaxPosition.y + Size.y);
                              Size.x, Size.y);
               break;
            }

         case SW_SHOWNOACTIVATE:
            Wnd->InternalPos.flags &= ~WPF_RESTORETOMAXIMIZED;
            /* fall through */
         case SW_SHOWNORMAL:
         case SW_RESTORE:
         case SW_SHOWDEFAULT: /* FIXME: should have its own handler */
            {
               //ERR("MinMaximize Restore\n");
               old_style = IntSetStyle( Wnd, 0, WS_MINIMIZE | WS_MAXIMIZE );
               if (old_style & WS_MINIMIZE)
               {
                  if (Wnd->InternalPos.flags & WPF_RESTORETOMAXIMIZED)
                  {
                     co_WinPosGetMinMaxInfo(Wnd, &Size, &wpl.ptMaxPosition, NULL, NULL);
                     IntSetStyle( Wnd, WS_MAXIMIZE, 0 );
                     SwpFlags |= SWP_STATECHANGED;
                     /*ERR("Restore to Max: %d,%d %dx%d\n",
                      wpl.ptMaxPosition.x, wpl.ptMaxPosition.y, Size.x, Size.y);
                     */
                     RECTL_vSetRect(NewPos, wpl.ptMaxPosition.x, wpl.ptMaxPosition.y,
//                                    wpl.ptMaxPosition.x + Size.x, wpl.ptMaxPosition.y + Size.y);
                                    Size.x, Size.y);
                     break;
                  }
                  else
                  {
                     *NewPos = wpl.rcNormalPosition;
                     /*ERR("Restore Max: %d,%d %dx%d\n",
                      NewPos->left, NewPos->top, NewPos->right - NewPos->left, NewPos->bottom - NewPos->top);
                      */
                     NewPos->right -= NewPos->left;
                     NewPos->bottom -= NewPos->top;
                     break;
                  }
               }
               else
               {
                  if (!(old_style & WS_MAXIMIZE))
                  {
                     break;
                  }
                  SwpFlags |= SWP_STATECHANGED;
                  Wnd->InternalPos.flags &= ~WPF_RESTORETOMAXIMIZED;
                  *NewPos = wpl.rcNormalPosition;
                     /*ERR("Restore Min: %d,%d %dx%d\n",
                      NewPos->left, NewPos->top, NewPos->right - NewPos->left, NewPos->bottom - NewPos->top);
                      */
                  NewPos->right -= NewPos->left;
                  NewPos->bottom -= NewPos->top;
                  break;
               }
            }
      }
   return SwpFlags;
}

BOOL
UserHasWindowEdge(DWORD Style, DWORD ExStyle)
{
   if (Style & WS_MINIMIZE)
      return TRUE;
   if (ExStyle & WS_EX_DLGMODALFRAME)
      return TRUE;
   if (ExStyle & WS_EX_STATICEDGE)
      return FALSE;
   if (Style & WS_THICKFRAME)
      return TRUE;
   Style &= WS_CAPTION;
   if (Style == WS_DLGFRAME || Style == WS_CAPTION)
      return TRUE;
   return FALSE;
}

VOID FASTCALL
IntGetWindowBorderMeasures(PWND Wnd, UINT *cx, UINT *cy)
{
   if(HAS_DLGFRAME(Wnd->style, Wnd->ExStyle) && !(Wnd->style & WS_MINIMIZE))
   {
      *cx = UserGetSystemMetrics(SM_CXDLGFRAME);
      *cy = UserGetSystemMetrics(SM_CYDLGFRAME);
   }
   else
   {
      if(HAS_THICKFRAME(Wnd->style, Wnd->ExStyle)&& !(Wnd->style & WS_MINIMIZE))
      {
         *cx = UserGetSystemMetrics(SM_CXFRAME);
         *cy = UserGetSystemMetrics(SM_CYFRAME);
      }
      else if(HAS_THINFRAME(Wnd->style, Wnd->ExStyle))
      {
         *cx = UserGetSystemMetrics(SM_CXBORDER);
         *cy = UserGetSystemMetrics(SM_CYBORDER);
      }
      else
      {
         *cx = *cy = 0;
      }
   }
}

VOID
UserGetWindowBorders(DWORD Style, DWORD ExStyle, SIZE *Size, BOOL WithClient)
{
   DWORD Border = 0;

   if (UserHasWindowEdge(Style, ExStyle))
      Border += 2;
   else if (ExStyle & WS_EX_STATICEDGE)
      Border += 1;
   if ((ExStyle & WS_EX_CLIENTEDGE) && WithClient)
      Border += 2;
   if (Style & WS_CAPTION || ExStyle & WS_EX_DLGMODALFRAME)
      Border ++;
   Size->cx = Size->cy = Border;
   if ((Style & WS_THICKFRAME) && !(Style & WS_MINIMIZE))
   {
      Size->cx += UserGetSystemMetrics(SM_CXFRAME) - UserGetSystemMetrics(SM_CXDLGFRAME);
      Size->cy += UserGetSystemMetrics(SM_CYFRAME) - UserGetSystemMetrics(SM_CYDLGFRAME);
   }
   Size->cx *= UserGetSystemMetrics(SM_CXBORDER);
   Size->cy *= UserGetSystemMetrics(SM_CYBORDER);
}

BOOL WINAPI
UserAdjustWindowRectEx(LPRECT lpRect,
                       DWORD dwStyle,
                       BOOL bMenu,
                       DWORD dwExStyle)
{
   SIZE BorderSize;

   if (bMenu)
   {
      lpRect->top -= UserGetSystemMetrics(SM_CYMENU);
   }
   if ((dwStyle & WS_CAPTION) == WS_CAPTION)
   {
      if (dwExStyle & WS_EX_TOOLWINDOW)
         lpRect->top -= UserGetSystemMetrics(SM_CYSMCAPTION);
      else
         lpRect->top -= UserGetSystemMetrics(SM_CYCAPTION);
   }
   UserGetWindowBorders(dwStyle, dwExStyle, &BorderSize, TRUE);
   RECTL_vInflateRect(
      lpRect,
      BorderSize.cx,
      BorderSize.cy);

   return TRUE;
}

UINT FASTCALL
co_WinPosGetMinMaxInfo(PWND Window, POINT* MaxSize, POINT* MaxPos,
                       POINT* MinTrack, POINT* MaxTrack)
{
    MINMAXINFO MinMax;
    PMONITOR monitor;
    INT xinc, yinc;
    LONG style = Window->style;
    LONG adjustedStyle;
    LONG exstyle = Window->ExStyle;
    RECT rc;

    ASSERT_REFS_CO(Window);

    /* Compute default values */

    rc = Window->rcWindow;
    MinMax.ptReserved.x = rc.left;
    MinMax.ptReserved.y = rc.top;

    if ((style & WS_CAPTION) == WS_CAPTION)
        adjustedStyle = style & ~WS_BORDER; /* WS_CAPTION = WS_DLGFRAME | WS_BORDER */
    else
        adjustedStyle = style;

    if(Window->spwndParent)
        IntGetClientRect(Window->spwndParent, &rc);
    UserAdjustWindowRectEx(&rc, adjustedStyle, ((style & WS_POPUP) && Window->IDMenu), exstyle);

    xinc = -rc.left;
    yinc = -rc.top;

    MinMax.ptMaxSize.x = rc.right - rc.left;
    MinMax.ptMaxSize.y = rc.bottom - rc.top;
    if (style & (WS_DLGFRAME | WS_BORDER))
    {
        MinMax.ptMinTrackSize.x = UserGetSystemMetrics(SM_CXMINTRACK);
        MinMax.ptMinTrackSize.y = UserGetSystemMetrics(SM_CYMINTRACK);
    }
    else
    {
        MinMax.ptMinTrackSize.x = 2 * xinc;
        MinMax.ptMinTrackSize.y = 2 * yinc;
    }
    MinMax.ptMaxTrackSize.x = UserGetSystemMetrics(SM_CXMAXTRACK);
    MinMax.ptMaxTrackSize.y = UserGetSystemMetrics(SM_CYMAXTRACK);
    MinMax.ptMaxPosition.x = -xinc;
    MinMax.ptMaxPosition.y = -yinc;

   if (!EMPTYPOINT(Window->InternalPos.MaxPos)) MinMax.ptMaxPosition = Window->InternalPos.MaxPos;

    co_IntSendMessage(Window->head.h, WM_GETMINMAXINFO, 0, (LPARAM)&MinMax);

    /* if the app didn't change the values, adapt them for the current monitor */
    if ((monitor = UserGetPrimaryMonitor()))
    {
        RECT rc_work;

        rc_work = monitor->rcMonitor;

        if (style & WS_MAXIMIZEBOX)
        {
            if ((style & WS_CAPTION) == WS_CAPTION || !(style & (WS_CHILD | WS_POPUP)))
                rc_work = monitor->rcWork;
        }

        if (MinMax.ptMaxSize.x == UserGetSystemMetrics(SM_CXSCREEN) + 2 * xinc &&
            MinMax.ptMaxSize.y == UserGetSystemMetrics(SM_CYSCREEN) + 2 * yinc)
        {
            MinMax.ptMaxSize.x = (rc_work.right - rc_work.left) + 2 * xinc;
            MinMax.ptMaxSize.y = (rc_work.bottom - rc_work.top) + 2 * yinc;
        }
        if (MinMax.ptMaxPosition.x == -xinc && MinMax.ptMaxPosition.y == -yinc)
        {
            MinMax.ptMaxPosition.x = rc_work.left - xinc;
            MinMax.ptMaxPosition.y = rc_work.top - yinc;
        }
        if (MinMax.ptMaxSize.x >= (monitor->rcMonitor.right - monitor->rcMonitor.left) &&
            MinMax.ptMaxSize.y >= (monitor->rcMonitor.bottom - monitor->rcMonitor.top) )
            Window->state |= WNDS_MAXIMIZESTOMONITOR;
        else
            Window->state &= ~WNDS_MAXIMIZESTOMONITOR;
    }


   MinMax.ptMaxTrackSize.x = max(MinMax.ptMaxTrackSize.x,
                                 MinMax.ptMinTrackSize.x);
   MinMax.ptMaxTrackSize.y = max(MinMax.ptMaxTrackSize.y,
                                 MinMax.ptMinTrackSize.y);

   if (MaxSize)
      *MaxSize = MinMax.ptMaxSize;
   if (MaxPos)
      *MaxPos = MinMax.ptMaxPosition;
   if (MinTrack)
      *MinTrack = MinMax.ptMinTrackSize;
   if (MaxTrack)
      *MaxTrack = MinMax.ptMaxTrackSize;

   return 0; // FIXME: What does it return?
}

static
VOID FASTCALL
FixClientRect(PRECTL ClientRect, PRECTL WindowRect)
{
   if (ClientRect->left < WindowRect->left)
   {
      ClientRect->left = WindowRect->left;
   }
   else if (WindowRect->right < ClientRect->left)
   {
      ClientRect->left = WindowRect->right;
   }
   if (ClientRect->right < WindowRect->left)
   {
      ClientRect->right = WindowRect->left;
   }
   else if (WindowRect->right < ClientRect->right)
   {
      ClientRect->right = WindowRect->right;
   }
   if (ClientRect->top < WindowRect->top)
   {
      ClientRect->top = WindowRect->top;
   }
   else if (WindowRect->bottom < ClientRect->top)
   {
      ClientRect->top = WindowRect->bottom;
   }
   if (ClientRect->bottom < WindowRect->top)
   {
      ClientRect->bottom = WindowRect->top;
   }
   else if (WindowRect->bottom < ClientRect->bottom)
   {
      ClientRect->bottom = WindowRect->bottom;
   }
}

static
LONG FASTCALL
co_WinPosDoNCCALCSize(PWND Window, PWINDOWPOS WinPos,
                      RECT* WindowRect, RECT* ClientRect)
{
   PWND Parent;
   UINT wvrFlags = 0;

   ASSERT_REFS_CO(Window);

   /* Send WM_NCCALCSIZE message to get new client area */
   if ((WinPos->flags & (SWP_FRAMECHANGED | SWP_NOSIZE)) != SWP_NOSIZE)
   {
      NCCALCSIZE_PARAMS params;
      WINDOWPOS winposCopy;

      params.rgrc[0] = *WindowRect;
      params.rgrc[1] = Window->rcWindow;
      params.rgrc[2] = Window->rcClient;
      Parent = Window->spwndParent;
      if (0 != (Window->style & WS_CHILD) && Parent)
      {
         RECTL_vOffsetRect(&(params.rgrc[0]), - Parent->rcClient.left,
                          - Parent->rcClient.top);
         RECTL_vOffsetRect(&(params.rgrc[1]), - Parent->rcClient.left,
                          - Parent->rcClient.top);
         RECTL_vOffsetRect(&(params.rgrc[2]), - Parent->rcClient.left,
                          - Parent->rcClient.top);
      }
      params.lppos = &winposCopy;
      winposCopy = *WinPos;

      wvrFlags = co_IntSendMessage(Window->head.h, WM_NCCALCSIZE, TRUE, (LPARAM) &params);

      /* If the application send back garbage, ignore it */
      if (params.rgrc[0].left <= params.rgrc[0].right &&
          params.rgrc[0].top <= params.rgrc[0].bottom)
      {
         *ClientRect = params.rgrc[0];
         if ((Window->style & WS_CHILD) && Parent)
         {
            RECTL_vOffsetRect(ClientRect, Parent->rcClient.left,
                             Parent->rcClient.top);
         }
         FixClientRect(ClientRect, WindowRect);
      }

      /* FIXME: WVR_ALIGNxxx */

      if (ClientRect->left != Window->rcClient.left ||
          ClientRect->top != Window->rcClient.top)
      {
         WinPos->flags &= ~SWP_NOCLIENTMOVE;
      }

      if ((ClientRect->right - ClientRect->left !=
            Window->rcClient.right - Window->rcClient.left) ||
          (ClientRect->bottom - ClientRect->top !=
            Window->rcClient.bottom - Window->rcClient.top))
      {
         WinPos->flags &= ~SWP_NOCLIENTSIZE;
      }
   }
   else
   {
      if (!(WinPos->flags & SWP_NOMOVE) &&
          (ClientRect->left != Window->rcClient.left ||
           ClientRect->top != Window->rcClient.top))
      {
         WinPos->flags &= ~SWP_NOCLIENTMOVE;
      }
   }

   return wvrFlags;
}

static
BOOL FASTCALL
co_WinPosDoWinPosChanging(PWND Window,
                          PWINDOWPOS WinPos,
                          PRECTL WindowRect,
                          PRECTL ClientRect)
{
   INT X, Y;

   ASSERT_REFS_CO(Window);

   if (!(WinPos->flags & SWP_NOSENDCHANGING))
   {
      co_IntSendMessageNoWait(Window->head.h, WM_WINDOWPOSCHANGING, 0, (LPARAM) WinPos);
   }

   *WindowRect = Window->rcWindow;
   *ClientRect = Window->rcClient;

   if (!(WinPos->flags & SWP_NOSIZE))
   {
      WindowRect->right = WindowRect->left + WinPos->cx;
      WindowRect->bottom = WindowRect->top + WinPos->cy;
   }

   if (!(WinPos->flags & SWP_NOMOVE))
   {
      PWND Parent;
      X = WinPos->x;
      Y = WinPos->y;
      Parent = Window->spwndParent;
      if ((0 != (Window->style & WS_CHILD)) && Parent)
      {
         X += Parent->rcClient.left;
         Y += Parent->rcClient.top;
      }

      WindowRect->left = X;
      WindowRect->top = Y;
      WindowRect->right += X - Window->rcWindow.left;
      WindowRect->bottom += Y - Window->rcWindow.top;
      RECTL_vOffsetRect(ClientRect,
                       X - Window->rcWindow.left,
                       Y - Window->rcWindow.top);
   }

   WinPos->flags |= SWP_NOCLIENTMOVE | SWP_NOCLIENTSIZE;

   return TRUE;
}

/*
 * Fix Z order taking into account owned popups -
 * basically we need to maintain them above the window that owns them
 *
 * FIXME: hide/show owned popups when owner visibility changes.
 *
 * ReactOS: See bug 6751 and 7228.
 */
static
HWND FASTCALL
WinPosDoOwnedPopups(PWND Window, HWND hWndInsertAfter)
{
   HWND *List = NULL;
   HWND Owner;
   LONG Style;
   PWND DesktopWindow, ChildObject;
   int i;

   Owner = Window->spwndOwner ? Window->spwndOwner->head.h : NULL;
   Style = Window->style;

   if ((Style & WS_POPUP) && Owner)
   {
      /* Make sure this popup stays above the owner */
      HWND hWndLocalPrev = HWND_TOPMOST;

      if (hWndInsertAfter != HWND_TOPMOST)
      {
         DesktopWindow = UserGetDesktopWindow();
         List = IntWinListChildren(DesktopWindow);

         if (List != NULL)
         {
            for (i = 0; List[i]; i++)
            {
               if (List[i] == Owner)
                  break;
               if (HWND_TOP == hWndInsertAfter)
               {
                  ChildObject = ValidateHwndNoErr(List[i]);
                  if (NULL != ChildObject)
                  {
                     if (0 == (ChildObject->ExStyle & WS_EX_TOPMOST))
                     {
                        break;
                     }
                  }
               }
               if (List[i] != Window->head.h)
                  hWndLocalPrev = List[i];
               if (hWndLocalPrev == hWndInsertAfter)
                  break;
            }
            hWndInsertAfter = hWndLocalPrev;
         }
      }
   }
   else if (Style & WS_CHILD)
   {
      return hWndInsertAfter;
   }

   if (!List)
   {
      DesktopWindow = UserGetDesktopWindow();
      List = IntWinListChildren(DesktopWindow);
   }
   if (List != NULL)
   {
      for (i = 0; List[i]; i++)
      {
         PWND Wnd;

         if (List[i] == UserHMGetHandle(Window))
            break;

         if (!(Wnd = ValidateHwndNoErr(List[i])))
            continue;

         if (Wnd->style & WS_POPUP && Wnd->spwndOwner == Window)
         {
            USER_REFERENCE_ENTRY Ref;
            UserRefObjectCo(Wnd, &Ref);

            co_WinPosSetWindowPos(Wnd, hWndInsertAfter, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING| SWP_DEFERERASE);

            UserDerefObjectCo(Wnd);

            hWndInsertAfter = List[i];
         }
      }
      ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
   }

   return hWndInsertAfter;
}

/***********************************************************************
 *      WinPosInternalMoveWindow
 *
 * Update WindowRect and ClientRect of Window and all of its children
 * We keep both WindowRect and ClientRect in screen coordinates internally
 */
static
VOID FASTCALL
WinPosInternalMoveWindow(PWND Window, INT MoveX, INT MoveY)
{
   PWND Child;

   ASSERT(Window != Window->spwndChild);

   Window->rcWindow.left += MoveX;
   Window->rcWindow.right += MoveX;
   Window->rcWindow.top += MoveY;
   Window->rcWindow.bottom += MoveY;

   Window->rcClient.left += MoveX;
   Window->rcClient.right += MoveX;
   Window->rcClient.top += MoveY;
   Window->rcClient.bottom += MoveY;

   for(Child = Window->spwndChild; Child; Child = Child->spwndNext)
   {
      WinPosInternalMoveWindow(Child, MoveX, MoveY);
   }
}

/*
 * WinPosFixupSWPFlags
 *
 * Fix redundant flags and values in the WINDOWPOS structure.
 */
static
BOOL FASTCALL
WinPosFixupFlags(WINDOWPOS *WinPos, PWND Wnd)
{
   if (Wnd->style & WS_VISIBLE)
   {
      WinPos->flags &= ~SWP_SHOWWINDOW;
   }
   else
   {
      WinPos->flags &= ~SWP_HIDEWINDOW;
      if (!(WinPos->flags & SWP_SHOWWINDOW))
         WinPos->flags |= SWP_NOREDRAW;
   }

   WinPos->cx = max(WinPos->cx, 0);
   WinPos->cy = max(WinPos->cy, 0);

   /* Check for right size */
   if (Wnd->rcWindow.right - Wnd->rcWindow.left == WinPos->cx &&
       Wnd->rcWindow.bottom - Wnd->rcWindow.top == WinPos->cy)
   {
      WinPos->flags |= SWP_NOSIZE;
   }

   /* Check for right position */
   if (Wnd->rcWindow.left == WinPos->x &&
       Wnd->rcWindow.top == WinPos->y)
   {
      WinPos->flags |= SWP_NOMOVE;
   }

   if (WinPos->hwnd == UserGetForegroundWindow())
   {
      WinPos->flags |= SWP_NOACTIVATE;   /* Already active */
   }
   else
      if ((Wnd->style & (WS_POPUP | WS_CHILD)) != WS_CHILD)
      {
         /* Bring to the top when activating */
         if (!(WinPos->flags & SWP_NOACTIVATE))
         {
            WinPos->flags &= ~SWP_NOZORDER;
            WinPos->hwndInsertAfter = (0 != (Wnd->ExStyle & WS_EX_TOPMOST) ?
                                       HWND_TOPMOST : HWND_TOP);
            return TRUE;
         }
      }

   /* Check hwndInsertAfter */
   if (!(WinPos->flags & SWP_NOZORDER))
   {
      /* Fix sign extension */
      if (WinPos->hwndInsertAfter == (HWND)0xffff)
      {
         WinPos->hwndInsertAfter = HWND_TOPMOST;
      }
      else if (WinPos->hwndInsertAfter == (HWND)0xfffe)
      {
         WinPos->hwndInsertAfter = HWND_NOTOPMOST;
      }

      if (WinPos->hwndInsertAfter == HWND_NOTOPMOST)
      {
         if (!(Wnd->ExStyle & WS_EX_TOPMOST))
            WinPos->flags |= SWP_NOZORDER;

         WinPos->hwndInsertAfter = HWND_TOP;
      }
      else if (HWND_TOP == WinPos->hwndInsertAfter
               && 0 != (Wnd->ExStyle & WS_EX_TOPMOST))
      {
         /* Keep it topmost when it's already topmost */
         WinPos->hwndInsertAfter = HWND_TOPMOST;
      }

      /* hwndInsertAfter must be a sibling of the window */
      if (HWND_TOPMOST != WinPos->hwndInsertAfter
            && HWND_TOP != WinPos->hwndInsertAfter
            && HWND_NOTOPMOST != WinPos->hwndInsertAfter
            && HWND_BOTTOM != WinPos->hwndInsertAfter)
      {
         PWND InsAfterWnd;

         InsAfterWnd = ValidateHwndNoErr(WinPos->hwndInsertAfter);
         if(!InsAfterWnd)
         {
             return TRUE;
         }

         if (InsAfterWnd->spwndParent != Wnd->spwndParent)
         {
            /* Note from wine User32 Win test_SetWindowPos:
               "Returns TRUE also for windows that are not siblings"
               "Does not seem to do anything even without passing flags, still returns TRUE"
               "Same thing the other way around."
               ".. and with these windows."
             */
            return FALSE;
         }
         else
         {
            /*
             * We don't need to change the Z order of hwnd if it's already
             * inserted after hwndInsertAfter or when inserting hwnd after
             * itself.
             */
            if ((WinPos->hwnd == WinPos->hwndInsertAfter) ||
                ((InsAfterWnd->spwndNext) && (WinPos->hwnd == InsAfterWnd->spwndNext->head.h)))
            {
               WinPos->flags |= SWP_NOZORDER;
            }
         }
      }
   }

   return TRUE;
}

/* x and y are always screen relative */
BOOLEAN FASTCALL
co_WinPosSetWindowPos(
   PWND Window,
   HWND WndInsertAfter,
   INT x,
   INT y,
   INT cx,
   INT cy,
   UINT flags
   )
{
   WINDOWPOS WinPos;
   RECTL NewWindowRect;
   RECTL NewClientRect;
   PROSRGNDATA VisRgn;
   HRGN VisBefore = NULL;
   HRGN VisAfter = NULL;
   HRGN DirtyRgn = NULL;
   HRGN ExposedRgn = NULL;
   HRGN CopyRgn = NULL;
   ULONG WvrFlags = 0;
   RECTL OldWindowRect, OldClientRect;
   int RgnType;
   HDC Dc;
   RECTL CopyRect;
   PWND Ancestor;
   BOOL bPointerInWindow;
   BOOL bNoTopMost;

   ASSERT_REFS_CO(Window);

   /* FIXME: Get current active window from active queue. */

   bPointerInWindow = IntPtInWindow(Window, gpsi->ptCursor.x, gpsi->ptCursor.y);

   WinPos.hwnd = Window->head.h;
   WinPos.hwndInsertAfter = WndInsertAfter;
   WinPos.x = x;
   WinPos.y = y;
   WinPos.cx = cx;
   WinPos.cy = cy;
   WinPos.flags = flags;

   co_WinPosDoWinPosChanging(Window, &WinPos, &NewWindowRect, &NewClientRect);

   // HWND_NOTOPMOST is redirected in WinPosFixupFlags.
   bNoTopMost = WndInsertAfter == HWND_NOTOPMOST;

   /* Does the window still exist? */
   if (!IntIsWindow(WinPos.hwnd))
   {
      TRACE("WinPosSetWindowPos: Invalid handle 0x%p!\n",WinPos.hwnd);
      EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
      return FALSE;
   }

   /* Fix up the flags. */
   if (!WinPosFixupFlags(&WinPos, Window))
   {
      // See Note.
      return TRUE;
   }

   Ancestor = UserGetAncestor(Window, GA_PARENT);
   if ( (WinPos.flags & (SWP_NOZORDER | SWP_HIDEWINDOW | SWP_SHOWWINDOW)) != SWP_NOZORDER &&
         Ancestor && Ancestor->head.h == IntGetDesktopWindow() )
   {
      WinPos.hwndInsertAfter = WinPosDoOwnedPopups(Window, WinPos.hwndInsertAfter);
   }

   if (!(WinPos.flags & SWP_NOREDRAW))
   {
      /* Compute the visible region before the window position is changed */
      if (!(WinPos.flags & SWP_SHOWWINDOW) &&
           (WinPos.flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_HIDEWINDOW | SWP_FRAMECHANGED)) !=
            (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER))
      {
         VisBefore = VIS_ComputeVisibleRegion(Window, FALSE, FALSE,
                                              (Window->style & WS_CLIPSIBLINGS) ? TRUE : FALSE);
         VisRgn = NULL;

         if ( VisBefore != NULL &&
             (VisRgn = (PROSRGNDATA)RGNOBJAPI_Lock(VisBefore, NULL)) &&
              REGION_Complexity(VisRgn) == NULLREGION )
         {
            RGNOBJAPI_Unlock(VisRgn);
            GreDeleteObject(VisBefore);
            VisBefore = NULL;
         }
         else if(VisRgn)
         {
            RGNOBJAPI_Unlock(VisRgn);
            NtGdiOffsetRgn(VisBefore, -Window->rcWindow.left, -Window->rcWindow.top);
         }
      }
   }

   WvrFlags = co_WinPosDoNCCALCSize(Window, &WinPos, &NewWindowRect, &NewClientRect);

   TRACE("co_WinPosDoNCCALCSize returned %d\n", WvrFlags);

   /* Validate link windows. (also take into account shell window in hwndShellWindow) */
   if (!(WinPos.flags & SWP_NOZORDER) && WinPos.hwnd != UserGetShellWindow())
   {
      //// Fix bug 6751 & 7228 see WinPosDoOwnedPopups wine Fixme.
      PWND ParentWindow;
      PWND Sibling;
      PWND InsertAfterWindow;

      if ((ParentWindow = Window->spwndParent)) // Must have a Parent window!
      {
         if (WinPos.hwndInsertAfter == HWND_TOPMOST)
         {
            InsertAfterWindow = NULL;
         }
         else if ( WinPos.hwndInsertAfter == HWND_TOP )
         {
            InsertAfterWindow = NULL;

            Sibling = ParentWindow->spwndChild;

            while ( Sibling && Sibling->ExStyle & WS_EX_TOPMOST )
            {
               InsertAfterWindow = Sibling;
               Sibling = Sibling->spwndNext;
            }
         }
         else if (WinPos.hwndInsertAfter == HWND_BOTTOM)
         {
            if (ParentWindow->spwndChild)
            {
               InsertAfterWindow = ParentWindow->spwndChild;

               if(InsertAfterWindow)
               {
                  while (InsertAfterWindow->spwndNext)
                     InsertAfterWindow = InsertAfterWindow->spwndNext;
               }
            }
            else
               InsertAfterWindow = NULL;
         }
         else
            InsertAfterWindow = IntGetWindowObject(WinPos.hwndInsertAfter);
         /* Do nothing if hwndInsertAfter is HWND_BOTTOM and Window is already
            the last window */
         if (InsertAfterWindow != Window)
         {
            IntUnlinkWindow(Window);
            IntLinkWindow(Window, InsertAfterWindow);
         }

         if ( ( WinPos.hwndInsertAfter == HWND_TOPMOST ||
               ( Window->ExStyle & WS_EX_TOPMOST && Window->spwndPrev && Window->spwndPrev->ExStyle & WS_EX_TOPMOST ) ||
               ( Window->spwndNext && Window->spwndNext->ExStyle & WS_EX_TOPMOST ) ) &&
               !bNoTopMost )
         {
            Window->ExStyle |= WS_EX_TOPMOST;
         }
         else
         {
            Window->ExStyle &= ~ WS_EX_TOPMOST;
         }
      }
      ////
   }

   OldWindowRect = Window->rcWindow;
   OldClientRect = Window->rcClient;

   if (OldClientRect.bottom - OldClientRect.top ==
         NewClientRect.bottom - NewClientRect.top)
   {
      WvrFlags &= ~WVR_VREDRAW;
   }

   if (OldClientRect.right - OldClientRect.left ==
         NewClientRect.right - NewClientRect.left)
   {
      WvrFlags &= ~WVR_HREDRAW;
   }

   /* FIXME: Actually do something with WVR_VALIDRECTS */

   if (NewClientRect.left != OldClientRect.left ||
       NewClientRect.top  != OldClientRect.top)
   {
      WinPosInternalMoveWindow(Window,
                               NewClientRect.left - OldClientRect.left,
                               NewClientRect.top - OldClientRect.top);
   }

   Window->rcWindow = NewWindowRect;
   Window->rcClient = NewClientRect;

   /* erase parent when hiding or resizing child */
   if (WinPos.flags & SWP_HIDEWINDOW)
   {
      /* Clear the update region */
      co_UserRedrawWindow( Window,
                           NULL,
                           0,
                           RDW_VALIDATE | RDW_NOFRAME | RDW_NOERASE | RDW_NOINTERNALPAINT | RDW_ALLCHILDREN);

      if (Window->spwndParent == UserGetDesktopWindow())
         co_IntShellHookNotify(HSHELL_WINDOWDESTROYED, (WPARAM)Window->head.h, 0);

      Window->style &= ~WS_VISIBLE; //IntSetStyle( Window, 0, WS_VISIBLE );
      IntNotifyWinEvent(EVENT_OBJECT_HIDE, Window, OBJID_WINDOW, CHILDID_SELF, WEF_SETBYWNDPTI);
   }
   else if (WinPos.flags & SWP_SHOWWINDOW)
   {
      if (Window->spwndParent == UserGetDesktopWindow())
         co_IntShellHookNotify(HSHELL_WINDOWCREATED, (WPARAM)Window->head.h, 0);

      Window->style |= WS_VISIBLE; //IntSetStyle( Window, WS_VISIBLE, 0 );
      IntNotifyWinEvent(EVENT_OBJECT_SHOW, Window, OBJID_WINDOW, CHILDID_SELF, WEF_SETBYWNDPTI);
   }

   if (Window->hrgnUpdate != NULL && Window->hrgnUpdate != HRGN_WINDOW)
   {
      NtGdiOffsetRgn(Window->hrgnUpdate,
                     NewWindowRect.left - OldWindowRect.left,
                     NewWindowRect.top - OldWindowRect.top);
   }

   DceResetActiveDCEs(Window); // For WS_VISIBLE changes.

   if (!(WinPos.flags & SWP_NOREDRAW))
   {
      /* Determine the new visible region */
      VisAfter = VIS_ComputeVisibleRegion(Window, FALSE, FALSE,
                                          (Window->style & WS_CLIPSIBLINGS) ? TRUE : FALSE);
      VisRgn = NULL;

      if ( VisAfter != NULL &&
          (VisRgn = (PROSRGNDATA)RGNOBJAPI_Lock(VisAfter, NULL)) &&
           REGION_Complexity(VisRgn) == NULLREGION )
      {
         RGNOBJAPI_Unlock(VisRgn);
         GreDeleteObject(VisAfter);
         VisAfter = NULL;
      }
      else if(VisRgn)
      {
         RGNOBJAPI_Unlock(VisRgn);
         NtGdiOffsetRgn(VisAfter, -Window->rcWindow.left, -Window->rcWindow.top);
      }

      /*
       * Determine which pixels can be copied from the old window position
       * to the new. Those pixels must be visible in both the old and new
       * position. Also, check the class style to see if the windows of this
       * class need to be completely repainted on (horizontal/vertical) size
       * change.
       */
      if ( VisBefore != NULL &&
           VisAfter != NULL &&
          !(WinPos.flags & SWP_NOCOPYBITS) &&
          ((WinPos.flags & SWP_NOSIZE) || !(WvrFlags & WVR_REDRAW)) &&
          !(Window->ExStyle & WS_EX_TRANSPARENT) )
      {
         CopyRgn = IntSysCreateRectRgn(0, 0, 0, 0);
         RgnType = NtGdiCombineRgn(CopyRgn, VisAfter, VisBefore, RGN_AND);

         /*
          * If this is (also) a window resize, the whole nonclient area
          * needs to be repainted. So we limit the copy to the client area,
          * 'cause there is no use in copying it (would possibly cause
          * "flashing" too). However, if the copy region is already empty,
          * we don't have to crop (can't take anything away from an empty
          * region...)
          */
         if (!(WinPos.flags & SWP_NOSIZE) &&
               RgnType != ERROR &&
               RgnType != NULLREGION )
         {
            PROSRGNDATA pCopyRgn;
            RECTL ORect = OldClientRect;
            RECTL NRect = NewClientRect;
            RECTL_vOffsetRect(&ORect, - OldWindowRect.left, - OldWindowRect.top);
            RECTL_vOffsetRect(&NRect, - NewWindowRect.left, - NewWindowRect.top);
            RECTL_bIntersectRect(&CopyRect, &ORect, &NRect);
            pCopyRgn = RGNOBJAPI_Lock(CopyRgn, NULL);
            REGION_CropAndOffsetRegion(pCopyRgn, pCopyRgn, &CopyRect, NULL);
            RGNOBJAPI_Unlock(pCopyRgn);
         }

         /* No use in copying bits which are in the update region. */
         if (Window->hrgnUpdate != NULL)
         {
            NtGdiOffsetRgn(CopyRgn, NewWindowRect.left, NewWindowRect.top);
            NtGdiCombineRgn(CopyRgn, CopyRgn, Window->hrgnUpdate, RGN_DIFF);
            NtGdiOffsetRgn(CopyRgn, -NewWindowRect.left, -NewWindowRect.top);
         }

         /*
          * Now, get the bounding box of the copy region. If it's empty
          * there's nothing to copy. Also, it's no use copying bits onto
          * themselves.
          */
         if ( (VisRgn = (PROSRGNDATA)RGNOBJAPI_Lock(CopyRgn, NULL)) &&
               REGION_GetRgnBox(VisRgn, &CopyRect) == NULLREGION)
         {
            /* Nothing to copy, clean up */
            RGNOBJAPI_Unlock(VisRgn);
            GreDeleteObject(CopyRgn);
            CopyRgn = NULL;
         }
         else if (OldWindowRect.left != NewWindowRect.left ||
                  OldWindowRect.top != NewWindowRect.top)
         {
            if(VisRgn)
            {
               RGNOBJAPI_Unlock(VisRgn);
            }

          /*
           * Small trick here: there is no function to bitblt a region. So
           * we set the region as the clipping region, take the bounding box
           * of the region and bitblt that. Since nothing outside the clipping
           * region is copied, this has the effect of bitblt'ing the region.
           *
           * Since NtUserGetDCEx takes ownership of the clip region, we need
           * to create a copy of CopyRgn and pass that. We need CopyRgn later
           */
            NtGdiOffsetRgn(CopyRgn, NewWindowRect.left, NewWindowRect.top);
            Dc = UserGetDCEx( Window,
                              CopyRgn,
                              DCX_WINDOW|DCX_CACHE|DCX_INTERSECTRGN|DCX_CLIPSIBLINGS|DCX_KEEPCLIPRGN);
            NtGdiBitBlt( Dc,
                         CopyRect.left, CopyRect.top,
                         CopyRect.right - CopyRect.left,
                         CopyRect.bottom - CopyRect.top,
                         Dc,
                         CopyRect.left + (OldWindowRect.left - NewWindowRect.left),
                         CopyRect.top + (OldWindowRect.top - NewWindowRect.top),
                         SRCCOPY,
                         0,
                         0);

            UserReleaseDC(Window, Dc, FALSE);
            IntValidateParent(Window, CopyRgn, FALSE);
            NtGdiOffsetRgn(CopyRgn, -NewWindowRect.left, -NewWindowRect.top);
         }
         else if(VisRgn)
         {
            RGNOBJAPI_Unlock(VisRgn);
         }
      }
      else
      {
         CopyRgn = NULL;
      }

      /* We need to redraw what wasn't visible before */
      if (VisAfter != NULL)
      {
         DirtyRgn = IntSysCreateRectRgn(0, 0, 0, 0);
         if (CopyRgn != NULL)
         {
            RgnType = NtGdiCombineRgn(DirtyRgn, VisAfter, CopyRgn, RGN_DIFF);
         }
         else
         {
            RgnType = NtGdiCombineRgn(DirtyRgn, VisAfter, 0, RGN_COPY);
         }
         if (RgnType != ERROR && RgnType != NULLREGION)
         {
        /* old code
            NtGdiOffsetRgn(DirtyRgn, Window->rcWindow.left, Window->rcWindow.top);
            IntInvalidateWindows( Window,
                                  DirtyRgn,
               RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
         }
         GreDeleteObject(DirtyRgn);
         */

            PWND Parent = Window->spwndParent;

            NtGdiOffsetRgn( DirtyRgn,
                            Window->rcWindow.left,
                            Window->rcWindow.top);
            if ( (Window->style & WS_CHILD) &&
                 (Parent) &&
                !(Parent->style & WS_CLIPCHILDREN))
            {
               IntInvalidateWindows( Parent,
                                     DirtyRgn,
                                     RDW_ERASE | RDW_INVALIDATE);
               co_IntPaintWindows(Parent, RDW_ERASENOW, FALSE);
            }
            else
            {
                IntInvalidateWindows( Window,
                                      DirtyRgn,
                    RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
         }
         GreDeleteObject(DirtyRgn);
      }

      if (CopyRgn != NULL)
      {
         GreDeleteObject(CopyRgn);
      }

      /* Expose what was covered before but not covered anymore */
      if (VisBefore != NULL)
      {
         ExposedRgn = IntSysCreateRectRgn(0, 0, 0, 0);
         RgnType = NtGdiCombineRgn(ExposedRgn, VisBefore, NULL, RGN_COPY);
         NtGdiOffsetRgn( ExposedRgn,
                         OldWindowRect.left - NewWindowRect.left,
                         OldWindowRect.top  - NewWindowRect.top);

         if (VisAfter != NULL)
            RgnType = NtGdiCombineRgn(ExposedRgn, ExposedRgn, VisAfter, RGN_DIFF);

         if (RgnType != ERROR && RgnType != NULLREGION)
         {
            co_VIS_WindowLayoutChanged(Window, ExposedRgn);
         }
         GreDeleteObject(ExposedRgn);
         GreDeleteObject(VisBefore);
      }

      if (VisAfter != NULL)
      {
         GreDeleteObject(VisAfter);
      }
   }

   if (!(WinPos.flags & (SWP_NOACTIVATE|SWP_HIDEWINDOW)))
   {
      if ((Window->style & (WS_CHILD | WS_POPUP)) == WS_CHILD)
      {
         co_IntSendMessageNoWait(WinPos.hwnd, WM_CHILDACTIVATE, 0, 0);
      }
      else
      {
         //ERR("SetWindowPos Set FG Window!\n");
         co_IntSetForegroundWindow(Window);
      }
   }

   /* And last, send the WM_WINDOWPOSCHANGED message */

   TRACE("\tstatus flags = %04x\n", WinPos.flags & SWP_AGG_STATUSFLAGS);

   if ((WinPos.flags & SWP_AGG_STATUSFLAGS) != SWP_AGG_NOPOSCHANGE)
   {
      /* WM_WINDOWPOSCHANGED is sent even if SWP_NOSENDCHANGING is set
         and always contains final window position.
       */
      WinPos.x = NewWindowRect.left;
      WinPos.y = NewWindowRect.top;
      WinPos.cx = NewWindowRect.right - NewWindowRect.left;
      WinPos.cy = NewWindowRect.bottom - NewWindowRect.top;
      co_IntSendMessageNoWait(WinPos.hwnd, WM_WINDOWPOSCHANGED, 0, (LPARAM) &WinPos);
   }

   if ( WinPos.flags & SWP_FRAMECHANGED  || WinPos.flags & SWP_STATECHANGED ||
      !(WinPos.flags & SWP_NOCLIENTSIZE) || !(WinPos.flags & SWP_NOCLIENTMOVE) )
   {
      PWND pWnd = ValidateHwndNoErr(WinPos.hwnd);
      if (pWnd)
         IntNotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, pWnd, OBJID_WINDOW, CHILDID_SELF, WEF_SETBYWNDPTI);
   }

   if(bPointerInWindow != IntPtInWindow(Window, gpsi->ptCursor.x, gpsi->ptCursor.y))
   {
      /* Generate mouse move message */
      MSG msg;
      msg.message = WM_MOUSEMOVE;
      msg.wParam = UserGetMouseButtonsState();
      msg.lParam = MAKELPARAM(gpsi->ptCursor.x, gpsi->ptCursor.y);
      msg.pt = gpsi->ptCursor;
      co_MsqInsertMouseMessage(&msg, 0, 0, TRUE);
   }

   return TRUE;
}

LRESULT FASTCALL
co_WinPosGetNonClientSize(PWND Window, RECT* WindowRect, RECT* ClientRect)
{
   LRESULT Result;

   ASSERT_REFS_CO(Window);

   *ClientRect = *WindowRect;
   Result = co_IntSendMessageNoWait(Window->head.h, WM_NCCALCSIZE, FALSE, (LPARAM) ClientRect);

   FixClientRect(ClientRect, WindowRect);

   return Result;
}

void FASTCALL
co_WinPosSendSizeMove(PWND Wnd)
{
    RECTL Rect;
    LPARAM lParam;
    WPARAM wParam = SIZE_RESTORED;

    IntGetClientRect(Wnd, &Rect);
    lParam = MAKELONG(Rect.right-Rect.left, Rect.bottom-Rect.top);

    Wnd->state &= ~WNDS_SENDSIZEMOVEMSGS;

    if (Wnd->style & WS_MAXIMIZE)
    {
        wParam = SIZE_MAXIMIZED;
    }
    else if (Wnd->style & WS_MINIMIZE)
    {
        wParam = SIZE_MINIMIZED;
        lParam = 0;
    }

    co_IntSendMessageNoWait(UserHMGetHandle(Wnd), WM_SIZE, wParam, lParam);

    if (Wnd->spwndParent == UserGetDesktopWindow()) // Wnd->spwndParent->fnid != FNID_DESKTOP )
       lParam = MAKELONG(Wnd->rcClient.left, Wnd->rcClient.top);
    else
       lParam = MAKELONG(Wnd->rcClient.left-Wnd->spwndParent->rcClient.left, Wnd->rcClient.top-Wnd->spwndParent->rcClient.top);

    co_IntSendMessageNoWait(UserHMGetHandle(Wnd), WM_MOVE, 0, lParam);

    IntEngWindowChanged(Wnd, WOC_RGN_CLIENT);
}

BOOLEAN FASTCALL
co_WinPosShowWindow(PWND Wnd, INT Cmd)
{
   BOOLEAN WasVisible;
   UINT Swp = 0, EventMsg = 0;
   RECTL NewPos;
   BOOLEAN ShowFlag;
   LONG style;
   PWND Parent;
   PTHREADINFO pti;
   BOOL ShowOwned = FALSE;
   //  HRGN VisibleRgn;

   ASSERT_REFS_CO(Wnd);

   pti = PsGetCurrentThreadWin32Thread();
   WasVisible = (Wnd->style & WS_VISIBLE) != 0;
   style = Wnd->style;

   switch (Cmd)
   {
      case SW_HIDE:
         {
            if (!WasVisible)
            {
               return(FALSE);
            }
            Swp |= SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE;
            if (Wnd != pti->MessageQueue->spwndActive)
               Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
            break;
         }

      case SW_FORCEMINIMIZE: /* FIXME: Does not work if thread is hung. */
      case SW_SHOWMINNOACTIVE:
         Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
         /* Fall through. */
      case SW_SHOWMINIMIZED:
         Swp |= SWP_SHOWWINDOW;
         /* Fall through. */
      case SW_MINIMIZE:
         {
            Swp |= SWP_NOACTIVATE;
            if (!(style & WS_MINIMIZE))
            {
               //IntShowOwnedPopups(Wnd, FALSE );

               // Fix wine Win test_SetFocus todo #1 & #2,
               if (Cmd == SW_SHOWMINIMIZED)
               {
                  if ((style & (WS_CHILD | WS_POPUP)) == WS_CHILD)
                     co_UserSetFocus(Wnd->spwndParent);
                  else
                     co_UserSetFocus(0);
               }

               Swp |= co_WinPosMinMaximize(Wnd, Cmd, &NewPos) |
                      SWP_FRAMECHANGED;

               EventMsg = EVENT_SYSTEM_MINIMIZESTART;
            }
            else
            {
               if (!WasVisible)
               {
                  Swp |= SWP_FRAMECHANGED;
               }
               else ////
                  return TRUE;
               Swp |= SWP_NOSIZE | SWP_NOMOVE;
            }
            break;
         }

      case SW_SHOWMAXIMIZED:
         {
            Swp |= SWP_SHOWWINDOW;
            if (!(style & WS_MAXIMIZE))
            {
               ShowOwned = TRUE;

               Swp |= co_WinPosMinMaximize(Wnd, SW_MAXIMIZE, &NewPos) |
                      SWP_FRAMECHANGED;

               EventMsg = EVENT_SYSTEM_MINIMIZEEND;
            }
            else
            {
               if (!WasVisible)
               {
                  Swp |= SWP_FRAMECHANGED;
               }
               else ////
                  return TRUE;
               Swp |= SWP_NOSIZE | SWP_NOMOVE;
            }
            break;
         }

      case SW_SHOWNA:
         Swp |= SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
         if (style & WS_CHILD && !(Wnd->ExStyle & WS_EX_MDICHILD)) Swp |= SWP_NOZORDER;
         break;
      case SW_SHOW:
         if (WasVisible) return(TRUE); // Nothing to do!
         Swp |= SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
         /* Don't activate the topmost window. */
         if (style & WS_CHILD && !(Wnd->ExStyle & WS_EX_MDICHILD)) Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
         break;

      case SW_SHOWNOACTIVATE:
         Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
         /* Fall through. */
      case SW_SHOWNORMAL:
      case SW_SHOWDEFAULT:
      case SW_RESTORE:
         if (!WasVisible) Swp |= SWP_SHOWWINDOW;
         if (style & (WS_MINIMIZE | WS_MAXIMIZE))
         {
            Swp |= co_WinPosMinMaximize(Wnd, Cmd, &NewPos) |
                   SWP_FRAMECHANGED;

            if (style & WS_MINIMIZE) EventMsg = EVENT_SYSTEM_MINIMIZEEND;
         }
         else
         {
            if (!WasVisible)
            {
               Swp |= SWP_FRAMECHANGED;
            }
            else ////
               return TRUE;
            Swp |= SWP_NOSIZE | SWP_NOMOVE;
         }
         if ( style & WS_CHILD &&
             !(Wnd->ExStyle & WS_EX_MDICHILD) &&
             !(Swp & SWP_STATECHANGED))
            Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
         break;

      default:
         return WasVisible;
   }

   ShowFlag = (Cmd != SW_HIDE);

   if ((ShowFlag != WasVisible || Cmd == SW_SHOWNA) && Cmd != SW_SHOWMAXIMIZED && !(Swp & SWP_STATECHANGED))
   {
      co_IntSendMessageNoWait(Wnd->head.h, WM_SHOWWINDOW, ShowFlag, 0);
      if (!(Wnd->state2 & WNDS2_WIN31COMPAT))
         co_IntSendMessageNoWait(Wnd->head.h, WM_SETVISIBLE, ShowFlag, 0);
      if (!VerifyWnd(Wnd)) return WasVisible;
   }

   /* We can't activate a child window */
   if ((Wnd->style & WS_CHILD) &&
       !(Wnd->ExStyle & WS_EX_MDICHILD) &&
       Cmd != SW_SHOWNA)
   {
      //ERR("SWP Child No active and ZOrder\n");
      Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
   }

#if 0 // Explorer issues with common controls. Someone does not know how CS_SAVEBITS works.
   if ((Wnd->style & (WS_POPUP|WS_CHILD)) != WS_CHILD &&
        Wnd->pcls->style & CS_SAVEBITS &&
        ((Cmd == SW_SHOW) || (Cmd == SW_NORMAL)))
   {
      co_IntSetActiveWindow(Wnd,NULL,FALSE,TRUE,FALSE);
      Swp |= SWP_NOACTIVATE | SWP_NOZORDER;
   }
#endif

   if (IsChildVisible(Wnd) || Swp & SWP_STATECHANGED)
   {
       TRACE("Child is Vis %s or State changed %s. ShowFlag %s\n",
             (IsChildVisible(Wnd) ? "TRUE" : "FALSE"), (Swp & SWP_STATECHANGED ? "TRUE" : "FALSE"),
             (ShowFlag ? "TRUE" : "FALSE"));
   co_WinPosSetWindowPos( Wnd,
                          0 != (Wnd->ExStyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_TOP,
                          NewPos.left,
                          NewPos.top,
                          NewPos.right, //NewPos.right - NewPos.left,
                          NewPos.bottom, //NewPos.bottom - NewPos.top,
                          LOWORD(Swp));
   }
   else
   {
      TRACE("Parent Vis?\n");
      /* if parent is not visible simply toggle WS_VISIBLE and return */
      if (ShowFlag) IntSetStyle( Wnd, WS_VISIBLE, 0 );
      else IntSetStyle( Wnd, 0, WS_VISIBLE );
   }

   if ( EventMsg ) IntNotifyWinEvent(EventMsg, Wnd, OBJID_WINDOW, CHILDID_SELF, WEF_SETBYWNDPTI);

   //if ( ShowOwned ) IntShowOwnedPopups(Wnd, TRUE );

   if ((Cmd == SW_HIDE) || (Cmd == SW_MINIMIZE))
   {
      if ( ( Wnd->spwndParent == UserGetDesktopWindow() && !ActivateOtherWindowMin(Wnd) ) ||
           // and Rule #1.
           ( Wnd == pti->MessageQueue->spwndActive && pti->MessageQueue == IntGetFocusMessageQueue() ) )
      {
         co_WinPosActivateOtherWindow(Wnd);
      }

      /* Revert focus to parent */
      if (Wnd == pti->MessageQueue->spwndFocus)
      {
         Parent = Wnd->spwndParent;
         if (Wnd->spwndParent == UserGetDesktopWindow()) Parent = 0;
         co_UserSetFocus(Parent);
      }
   }

   /* FIXME: Check for window destruction. */

   if ((Wnd->state & WNDS_SENDSIZEMOVEMSGS) &&
       !(Wnd->state2 & WNDS2_INDESTROY))
   {
        co_WinPosSendSizeMove(Wnd);
   }

   /* if previous state was minimized Windows sets focus to the window */
   if (style & WS_MINIMIZE)
   {
      co_UserSetFocus(Wnd);
      // Fix wine Win test_SetFocus todo #3,
      if (!(style & WS_CHILD)) co_IntSendMessageNoWait(UserHMGetHandle(Wnd), WM_ACTIVATE, WA_ACTIVE, 0);
   }
   return(WasVisible);
}

static
PWND FASTCALL
co_WinPosSearchChildren(
   PWND ScopeWin,
   POINT *Point,
   USHORT *HitTest
   )
{
    PWND pwndChild;
    HWND *List, *phWnd;

    if (!(ScopeWin->style & WS_VISIBLE))
    {
        return NULL;
    }

    if ((ScopeWin->style & WS_DISABLED))
    {
        return NULL;
    }

    if (!IntPtInWindow(ScopeWin, Point->x, Point->y))
    {
        return NULL;
    }

    UserReferenceObject(ScopeWin);

    if ( RECTL_bPointInRect(&ScopeWin->rcClient, Point->x, Point->y) )
    {
        List = IntWinListChildren(ScopeWin);
        if(List)
        {
            for (phWnd = List; *phWnd; ++phWnd)
            {
                if (!(pwndChild = ValidateHwndNoErr(*phWnd)))
                {
                    continue;
                }

                pwndChild = co_WinPosSearchChildren(pwndChild, Point, HitTest);

                if(pwndChild != NULL)
                {
                    /* We found a window. Don't send any more WM_NCHITTEST messages */
                    ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
                    UserDereferenceObject(ScopeWin);
                    return pwndChild;
                }
            }
            ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
        }
    }

    if (ScopeWin->head.pti == PsGetCurrentThreadWin32Thread())
    {
       *HitTest = (USHORT)co_IntSendMessage(ScopeWin->head.h, WM_NCHITTEST, 0,
                                            MAKELONG(Point->x, Point->y));
       if ((*HitTest) == (USHORT)HTTRANSPARENT)
       {
           UserDereferenceObject(ScopeWin);
           return NULL;
       }
    }
    else
       *HitTest = HTCLIENT;

    return ScopeWin;
}

PWND FASTCALL
co_WinPosWindowFromPoint(PWND ScopeWin, POINT *WinPoint, USHORT* HitTest)
{
   PWND Window;
   POINT Point = *WinPoint;
   USER_REFERENCE_ENTRY Ref;

   if( ScopeWin == NULL )
   {
       ScopeWin = UserGetDesktopWindow();
       if(ScopeWin == NULL)
           return NULL;
   }

   *HitTest = HTNOWHERE;

   ASSERT_REFS_CO(ScopeWin);
   UserRefObjectCo(ScopeWin, &Ref);

   Window = co_WinPosSearchChildren(ScopeWin, &Point, HitTest);

   UserDerefObjectCo(ScopeWin);
   if (Window)
       ASSERT_REFS_CO(Window);
   ASSERT_REFS_CO(ScopeWin);

   return Window;
}

PWND FASTCALL
IntRealChildWindowFromPoint(PWND Parent, LONG x, LONG y)
{
   POINTL Pt;
   HWND *List, *phWnd;
   PWND pwndHit = NULL;

   Pt.x = x;
   Pt.y = y;

   if (Parent != UserGetDesktopWindow())
   {
      Pt.x += Parent->rcClient.left;
      Pt.y += Parent->rcClient.top;
   }

   if (!IntPtInWindow(Parent, Pt.x, Pt.y)) return NULL;

   if ((List = IntWinListChildren(Parent)))
   {
      for (phWnd = List; *phWnd; phWnd++)
      {
         PWND Child;
         if ((Child = ValidateHwndNoErr(*phWnd)))
         {
            if ( Child->style & WS_VISIBLE && IntPtInWindow(Child, Pt.x, Pt.y) )
            {
               if ( Child->pcls->atomClassName != gpsi->atomSysClass[ICLS_BUTTON] ||
                   (Child->style & BS_TYPEMASK) != BS_GROUPBOX )
               {
                  ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
                  return Child;
               }
               pwndHit = Child;
            }
         }
      }
      ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
   }
   return pwndHit ? pwndHit : Parent;
}

PWND APIENTRY
IntChildWindowFromPointEx(PWND Parent, LONG x, LONG y, UINT uiFlags)
{
   POINTL Pt;
   HWND *List, *phWnd;
   PWND pwndHit = NULL;

   Pt.x = x;
   Pt.y = y;

   if (Parent != UserGetDesktopWindow())
   {
      if (Parent->ExStyle & WS_EX_LAYOUTRTL)
         Pt.x = Parent->rcClient.right - Pt.x;
      else
         Pt.x += Parent->rcClient.left;
      Pt.y += Parent->rcClient.top;
   }

   if (!IntPtInWindow(Parent, Pt.x, Pt.y)) return NULL;

   if ((List = IntWinListChildren(Parent)))
   {
      for (phWnd = List; *phWnd; phWnd++)
      {
         PWND Child;
         if ((Child = ValidateHwndNoErr(*phWnd)))
         {
            if (uiFlags & (CWP_SKIPINVISIBLE|CWP_SKIPDISABLED))
            {
               if (!(Child->style & WS_VISIBLE) && (uiFlags & CWP_SKIPINVISIBLE)) continue;
               if ((Child->style & WS_DISABLED) && (uiFlags & CWP_SKIPDISABLED)) continue;
            }

            if (uiFlags & CWP_SKIPTRANSPARENT)
            {
               if (Child->ExStyle & WS_EX_TRANSPARENT) continue;
            }

            if (IntPtInWindow(Child, Pt.x, Pt.y))
            {
               pwndHit = Child;
               break;
            }
         }
      }
      ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
   }
   return pwndHit ? pwndHit : Parent;
}

HDWP
FASTCALL
IntDeferWindowPos( HDWP hdwp,
                   HWND hwnd,
                   HWND hwndAfter,
                   INT x,
                   INT y,
                   INT cx,
                   INT cy,
                   UINT flags )
{
    PSMWP pDWP;
    int i;
    HDWP retvalue = hdwp;

    TRACE("hdwp %p, hwnd %p, after %p, %d,%d (%dx%d), flags %08x\n",
          hdwp, hwnd, hwndAfter, x, y, cx, cy, flags);

    if (flags & ~(SWP_NOSIZE | SWP_NOMOVE |
                  SWP_NOZORDER | SWP_NOREDRAW |
                  SWP_NOACTIVATE | SWP_NOCOPYBITS |
                  SWP_NOOWNERZORDER|SWP_SHOWWINDOW |
                  SWP_HIDEWINDOW | SWP_FRAMECHANGED))
    {
       EngSetLastError(ERROR_INVALID_PARAMETER);
       return NULL;
    }

    if (!(pDWP = (PSMWP)UserGetObject(gHandleTable, hdwp, TYPE_SETWINDOWPOS)))
    {
       EngSetLastError(ERROR_INVALID_DWP_HANDLE);
       return NULL;
    }

    for (i = 0; i < pDWP->ccvr; i++)
    {
        if (pDWP->acvr[i].pos.hwnd == hwnd)
        {
              /* Merge with the other changes */
            if (!(flags & SWP_NOZORDER))
            {
                pDWP->acvr[i].pos.hwndInsertAfter = hwndAfter;
            }
            if (!(flags & SWP_NOMOVE))
            {
                pDWP->acvr[i].pos.x = x;
                pDWP->acvr[i].pos.y = y;
            }
            if (!(flags & SWP_NOSIZE))
            {
                pDWP->acvr[i].pos.cx = cx;
                pDWP->acvr[i].pos.cy = cy;
            }
            pDWP->acvr[i].pos.flags &= flags | ~(SWP_NOSIZE | SWP_NOMOVE |
                                               SWP_NOZORDER | SWP_NOREDRAW |
                                               SWP_NOACTIVATE | SWP_NOCOPYBITS|
                                               SWP_NOOWNERZORDER);
            pDWP->acvr[i].pos.flags |= flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW |
                                              SWP_FRAMECHANGED);
            goto END;
        }
    }
    if (pDWP->ccvr >= pDWP->ccvrAlloc)
    {
        PCVR newpos = ExAllocatePoolWithTag(PagedPool, pDWP->ccvrAlloc * 2 * sizeof(CVR), USERTAG_SWP);
        if (!newpos)
        {
            retvalue = NULL;
            goto END;
        }
        RtlZeroMemory(newpos, pDWP->ccvrAlloc * 2 * sizeof(CVR));
        RtlCopyMemory(newpos, pDWP->acvr, pDWP->ccvrAlloc * sizeof(CVR));
        ExFreePoolWithTag(pDWP->acvr, USERTAG_SWP);
        pDWP->ccvrAlloc *= 2;
        pDWP->acvr = newpos;
    }
    pDWP->acvr[pDWP->ccvr].pos.hwnd = hwnd;
    pDWP->acvr[pDWP->ccvr].pos.hwndInsertAfter = hwndAfter;
    pDWP->acvr[pDWP->ccvr].pos.x = x;
    pDWP->acvr[pDWP->ccvr].pos.y = y;
    pDWP->acvr[pDWP->ccvr].pos.cx = cx;
    pDWP->acvr[pDWP->ccvr].pos.cy = cy;
    pDWP->acvr[pDWP->ccvr].pos.flags = flags;
    pDWP->acvr[pDWP->ccvr].hrgnClip = NULL;
    pDWP->acvr[pDWP->ccvr].hrgnInterMonitor = NULL;
    pDWP->ccvr++;
END:
    return retvalue;
}

BOOL FASTCALL IntEndDeferWindowPosEx( HDWP hdwp, BOOL sAsync )
{
    PSMWP pDWP;
    PCVR winpos;
    BOOL res = TRUE;
    int i;

    TRACE("%p\n", hdwp);

    if (!(pDWP = (PSMWP)UserGetObject(gHandleTable, hdwp, TYPE_SETWINDOWPOS)))
    {
       EngSetLastError(ERROR_INVALID_DWP_HANDLE);
       return FALSE;
    }

    for (i = 0, winpos = pDWP->acvr; res && i < pDWP->ccvr; i++, winpos++)
    {
        PWND pwnd;
        USER_REFERENCE_ENTRY Ref;

        TRACE("hwnd %p, after %p, %d,%d (%dx%d), flags %08x\n",
               winpos->pos.hwnd, winpos->pos.hwndInsertAfter, winpos->pos.x, winpos->pos.y,
               winpos->pos.cx, winpos->pos.cy, winpos->pos.flags);

        pwnd = ValidateHwndNoErr(winpos->pos.hwnd);
        if (!pwnd)
           continue;

        UserRefObjectCo(pwnd, &Ref);

        if ( sAsync )
        {
           LRESULT lRes;
           PWINDOWPOS ppos = ExAllocatePoolWithTag(PagedPool, sizeof(WINDOWPOS), USERTAG_SWP);
           if ( ppos )
           {
              *ppos = winpos->pos;
              /* Yes it's a pointer inside Win32k! */
              lRes = co_IntSendMessageNoWait( winpos->pos.hwnd, WM_ASYNC_SETWINDOWPOS, 0, (LPARAM)ppos);
              /* We handle this the same way as Event Hooks and Hooks. */
              if ( !lRes )
              {
                 ExFreePoolWithTag(ppos, USERTAG_SWP);
              }
           }
        }
        else
           res = co_WinPosSetWindowPos( pwnd,
                                        winpos->pos.hwndInsertAfter,
                                        winpos->pos.x,
                                        winpos->pos.y,
                                        winpos->pos.cx,
                                        winpos->pos.cy,
                                        winpos->pos.flags);

        // Hack to pass tests.... Must have some work to do so clear the error.
        if (res && (winpos->pos.flags & (SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER)) == SWP_NOZORDER )
           EngSetLastError(ERROR_SUCCESS);

        UserDerefObjectCo(pwnd);
    }
    ExFreePoolWithTag(pDWP->acvr, USERTAG_SWP);
    UserDereferenceObject(pDWP);
    UserDeleteObject(hdwp, TYPE_SETWINDOWPOS);
    return res;
}

/*
 * @implemented
 */
HWND APIENTRY
NtUserChildWindowFromPointEx(HWND hwndParent,
                             LONG x,
                             LONG y,
                             UINT uiFlags)
{
   PWND pwndParent;
   TRACE("Enter NtUserChildWindowFromPointEx\n");
   UserEnterExclusive();
   if ((pwndParent = UserGetWindowObject(hwndParent)))
   {
      pwndParent = IntChildWindowFromPointEx(pwndParent, x, y, uiFlags);
   }
   UserLeave();
   TRACE("Leave NtUserChildWindowFromPointEx\n");
   return pwndParent ? UserHMGetHandle(pwndParent) : NULL;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserEndDeferWindowPosEx(HDWP WinPosInfo,
                          DWORD Unknown1)
{
   BOOL Ret;
   TRACE("Enter NtUserEndDeferWindowPosEx\n");
   UserEnterExclusive();
   Ret = IntEndDeferWindowPosEx(WinPosInfo, (BOOL)Unknown1);
   TRACE("Leave NtUserEndDeferWindowPosEx, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
HDWP APIENTRY
NtUserDeferWindowPos(HDWP WinPosInfo,
                     HWND Wnd,
                     HWND WndInsertAfter,
                     int x,
                     int y,
                     int cx,
                     int cy,
                     UINT Flags)
{
   PWND pWnd, pWndIA;
   HDWP Ret = NULL;
   UINT Tmp = ~(SWP_ASYNCWINDOWPOS|SWP_DEFERERASE|SWP_NOSENDCHANGING|SWP_NOREPOSITION|
                SWP_NOCOPYBITS|SWP_HIDEWINDOW|SWP_SHOWWINDOW|SWP_FRAMECHANGED|
                SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOZORDER|SWP_NOMOVE|SWP_NOSIZE);

   TRACE("Enter NtUserDeferWindowPos\n");
   UserEnterExclusive();

   if ( Flags & Tmp )
   {
      EngSetLastError(ERROR_INVALID_FLAGS);
      goto Exit;
   }

   pWnd = UserGetWindowObject(Wnd);
   if ( !pWnd ||                           // FIXME:
         pWnd == UserGetDesktopWindow() || // pWnd->fnid == FNID_DESKTOP
         pWnd == UserGetMessageWindow() )  // pWnd->fnid == FNID_MESSAGEWND
   {
      goto Exit;
   }

   if ( WndInsertAfter &&
        WndInsertAfter != HWND_BOTTOM &&
        WndInsertAfter != HWND_TOPMOST &&
        WndInsertAfter != HWND_NOTOPMOST )
   {
      pWndIA = UserGetWindowObject(WndInsertAfter);
      if ( !pWndIA ||
            pWndIA == UserGetDesktopWindow() ||
            pWndIA == UserGetMessageWindow() )
      {
         goto Exit;
      }
   }

   Ret = IntDeferWindowPos(WinPosInfo, Wnd, WndInsertAfter, x, y, cx, cy, Flags);

Exit:
   TRACE("Leave NtUserDeferWindowPos, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
DWORD APIENTRY
NtUserGetInternalWindowPos( HWND hWnd,
                            LPRECT rectWnd,
                            LPPOINT ptIcon)
{
   PWND Window;
   DWORD Ret = 0;
   BOOL Hit = FALSE;
   WINDOWPLACEMENT wndpl;

   UserEnterShared();

   if (!(Window = UserGetWindowObject(hWnd)))
   {
      Hit = FALSE;
      goto Exit;
   }

   _SEH2_TRY
   {
       if(rectWnd)
       {
          ProbeForWrite(rectWnd,
                        sizeof(RECT),
                        1);
       }
       if(ptIcon)
       {
          ProbeForWrite(ptIcon,
                        sizeof(POINT),
                        1);
       }

   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
       SetLastNtError(_SEH2_GetExceptionCode());
       Hit = TRUE;
   }
   _SEH2_END;

   wndpl.length = sizeof(WINDOWPLACEMENT);

   if (IntGetWindowPlacement(Window, &wndpl) && !Hit)
   {
      _SEH2_TRY
      {
          if (rectWnd)
          {
             RtlCopyMemory(rectWnd, &wndpl.rcNormalPosition , sizeof(RECT));
          }
          if (ptIcon)
          {
             RtlCopyMemory(ptIcon, &wndpl.ptMinPosition, sizeof(POINT));
          }

      }
      _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
      {
          SetLastNtError(_SEH2_GetExceptionCode());
          Hit = TRUE;
      }
      _SEH2_END;

      if (!Hit) Ret = wndpl.showCmd;
   }
Exit:
   UserLeave();
   return Ret;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserGetWindowPlacement(HWND hWnd,
                         WINDOWPLACEMENT *lpwndpl)
{
   PWND Wnd;
   WINDOWPLACEMENT Safepl;
   NTSTATUS Status;
   DECLARE_RETURN(BOOL);

   TRACE("Enter NtUserGetWindowPlacement\n");
   UserEnterShared();

   if (!(Wnd = UserGetWindowObject(hWnd)))
   {
      RETURN( FALSE);
   }

   Status = MmCopyFromCaller(&Safepl, lpwndpl, sizeof(WINDOWPLACEMENT));
   if(!NT_SUCCESS(Status))
   {
      SetLastNtError(Status);
      RETURN( FALSE);
   }
   if(Safepl.length != sizeof(WINDOWPLACEMENT))
   {
      RETURN( FALSE);
   }

   IntGetWindowPlacement(Wnd, &Safepl);

   Status = MmCopyToCaller(lpwndpl, &Safepl, sizeof(WINDOWPLACEMENT));
   if(!NT_SUCCESS(Status))
   {
      SetLastNtError(Status);
      RETURN( FALSE);
   }

   RETURN( TRUE);

CLEANUP:
   TRACE("Leave NtUserGetWindowPlacement, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

DWORD
APIENTRY
NtUserMinMaximize(
    HWND hWnd,
    UINT cmd, // Wine SW_ commands
    BOOL Hide)
{
  PWND pWnd;

  TRACE("Enter NtUserMinMaximize\n");
  UserEnterExclusive();

  pWnd = UserGetWindowObject(hWnd);
  if ( !pWnd ||                           // FIXME:
        pWnd == UserGetDesktopWindow() || // pWnd->fnid == FNID_DESKTOP
        pWnd == UserGetMessageWindow() )  // pWnd->fnid == FNID_MESSAGEWND
  {
     goto Exit;
  }

  if ( cmd > SW_MAX || pWnd->state2 & WNDS2_INDESTROY)
  {
     EngSetLastError(ERROR_INVALID_PARAMETER);
     goto Exit;
  }

  cmd |= Hide ? SW_HIDE : 0;

  co_WinPosShowWindow(pWnd, cmd);

Exit:
  TRACE("Leave NtUserMinMaximize\n");
  UserLeave();
  return 0; // Always NULL?
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserMoveWindow(
   HWND hWnd,
   int X,
   int Y,
   int nWidth,
   int nHeight,
   BOOL bRepaint)
{
   return NtUserSetWindowPos(hWnd, 0, X, Y, nWidth, nHeight,
                             (bRepaint ? SWP_NOZORDER | SWP_NOACTIVATE :
                              SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW));
}

/*
 * @implemented
 */
HWND APIENTRY
NtUserRealChildWindowFromPoint(HWND Parent,
                               LONG x,
                               LONG y)
{
   PWND pwndParent;
   TRACE("Enter NtUserRealChildWindowFromPoint\n");
   UserEnterShared();
   if ((pwndParent = UserGetWindowObject(Parent)))
   {
      pwndParent = IntRealChildWindowFromPoint(pwndParent, x, y);
   }
   UserLeave();
   TRACE("Leave NtUserRealChildWindowFromPoint\n");
   return pwndParent ? UserHMGetHandle(pwndParent) : NULL;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserSetWindowPos(
   HWND hWnd,
   HWND hWndInsertAfter,
   int X,
   int Y,
   int cx,
   int cy,
   UINT uFlags)
{
   DECLARE_RETURN(BOOL);
   PWND Window, pWndIA;
   BOOL ret;
   USER_REFERENCE_ENTRY Ref;

   TRACE("Enter NtUserSetWindowPos\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)) || // FIXME:
         Window == UserGetDesktopWindow() ||    // pWnd->fnid == FNID_DESKTOP
         Window == UserGetMessageWindow() )     // pWnd->fnid == FNID_MESSAGEWND
   {
      ERR("NtUserSetWindowPos bad window handle!\n");
      RETURN(FALSE);
   }

   if ( hWndInsertAfter &&
        hWndInsertAfter != HWND_BOTTOM &&
        hWndInsertAfter != HWND_TOPMOST &&
        hWndInsertAfter != HWND_NOTOPMOST )
   {
      if (!(pWndIA = UserGetWindowObject(hWndInsertAfter)) ||
            pWndIA == UserGetDesktopWindow() ||
            pWndIA == UserGetMessageWindow() )
      {
         ERR("NtUserSetWindowPos bad insert window handle!\n");
         RETURN(FALSE);
      }
   }

   /* First make sure that coordinates are valid for WM_WINDOWPOSCHANGING */
   if (!(uFlags & SWP_NOMOVE))
   {
      if (X < -32768) X = -32768;
      else if (X > 32767) X = 32767;
      if (Y < -32768) Y = -32768;
      else if (Y > 32767) Y = 32767;
   }
   if (!(uFlags & SWP_NOSIZE))
   {
      if (cx < 0) cx = 0;
      else if (cx > 32767) cx = 32767;
      if (cy < 0) cy = 0;
      else if (cy > 32767) cy = 32767;
   }

   UserRefObjectCo(Window, &Ref);
   ret = co_WinPosSetWindowPos(Window, hWndInsertAfter, X, Y, cx, cy, uFlags);
   UserDerefObjectCo(Window);

   RETURN(ret);

CLEANUP:
   TRACE("Leave NtUserSetWindowPos, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * @implemented
 */
INT APIENTRY
NtUserSetWindowRgn(
   HWND hWnd,
   HRGN hRgn,
   BOOL bRedraw)
{
   HRGN hrgnCopy;
   PWND Window;
   INT flags = (SWP_NOCLIENTSIZE|SWP_NOCLIENTMOVE|SWP_NOACTIVATE|SWP_FRAMECHANGED|SWP_NOSIZE|SWP_NOMOVE);
   BOOLEAN Ret = FALSE;
   DECLARE_RETURN(INT);

   TRACE("Enter NtUserSetWindowRgn\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)) || // FIXME:
         Window == UserGetDesktopWindow() ||    // pWnd->fnid == FNID_DESKTOP
         Window == UserGetMessageWindow() )     // pWnd->fnid == FNID_MESSAGEWND
   {
      RETURN( 0);
   }

   if (hRgn) // The region will be deleted in user32.
   {
      if (GreIsHandleValid(hRgn))
      {
         hrgnCopy = IntSysCreateRectRgn(0, 0, 0, 0);
      /* The coordinates of a window's window region are relative to the
         upper-left corner of the window, not the client area of the window. */
         NtGdiCombineRgn( hrgnCopy, hRgn, 0, RGN_COPY);
      }
      else
         RETURN( 0);
   }
   else
   {
      hrgnCopy = NULL;
   }

   if (Window->hrgnClip)
   {
      /* Delete no longer needed region handle */
      IntGdiSetRegionOwner(Window->hrgnClip, GDI_OBJ_HMGR_POWNED);
      GreDeleteObject(Window->hrgnClip);
   }

   if (hrgnCopy)
   {
      /* Set public ownership */
      IntGdiSetRegionOwner(hrgnCopy, GDI_OBJ_HMGR_PUBLIC);
   }
   Window->hrgnClip = hrgnCopy;

   Ret = co_WinPosSetWindowPos(Window, HWND_TOP, 0, 0, 0, 0, bRedraw ? flags : (flags|SWP_NOREDRAW) );

   RETURN( (INT)Ret);

CLEANUP:
   TRACE("Leave NtUserSetWindowRgn, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * @implemented
 */
DWORD APIENTRY
NtUserSetInternalWindowPos(
   HWND    hwnd,
   UINT    showCmd,
   LPRECT  lprect,
   LPPOINT lppt)
{
   WINDOWPLACEMENT wndpl;
   UINT flags;
   PWND Wnd;
   RECT rect;
   POINT pt = {0};
   DECLARE_RETURN(BOOL);
   USER_REFERENCE_ENTRY Ref;

   TRACE("Enter NtUserSetWindowPlacement\n");
   UserEnterExclusive();

   if (!(Wnd = UserGetWindowObject(hwnd)) || // FIXME:
         Wnd == UserGetDesktopWindow() ||    // pWnd->fnid == FNID_DESKTOP
         Wnd == UserGetMessageWindow() )     // pWnd->fnid == FNID_MESSAGEWND
   {
      RETURN( FALSE);
   }

   _SEH2_TRY
   {
      if (lppt)
      {
         ProbeForRead(lppt, sizeof(POINT), 1);
         RtlCopyMemory(&pt, lppt, sizeof(POINT));
      }
      if (lprect)
      {
         ProbeForRead(lprect, sizeof(RECT), 1);
         RtlCopyMemory(&rect, lprect, sizeof(RECT));
      }
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      SetLastNtError(_SEH2_GetExceptionCode());
      _SEH2_YIELD(RETURN( FALSE));
   }
   _SEH2_END

   wndpl.length  = sizeof(wndpl);
   wndpl.showCmd = showCmd;
   wndpl.flags = flags = 0;

   if ( lppt )
   {
      flags |= PLACE_MIN;
      wndpl.flags |= WPF_SETMINPOSITION;
      wndpl.ptMinPosition = pt;
   }
   if ( lprect )
   {
      flags |= PLACE_RECT;
      wndpl.rcNormalPosition = rect;
   }

   UserRefObjectCo(Wnd, &Ref);
   IntSetWindowPlacement(Wnd, &wndpl, flags);
   UserDerefObjectCo(Wnd);
   RETURN(TRUE);

CLEANUP:
   TRACE("Leave NtUserSetWindowPlacement, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserSetWindowPlacement(HWND hWnd,
                         WINDOWPLACEMENT *lpwndpl)
{
   PWND Wnd;
   WINDOWPLACEMENT Safepl;
   UINT Flags;
   DECLARE_RETURN(BOOL);
   USER_REFERENCE_ENTRY Ref;

   TRACE("Enter NtUserSetWindowPlacement\n");
   UserEnterExclusive();

   if (!(Wnd = UserGetWindowObject(hWnd)) || // FIXME:
        Wnd == UserGetDesktopWindow() ||     // pWnd->fnid == FNID_DESKTOP
        Wnd == UserGetMessageWindow() )      // pWnd->fnid == FNID_MESSAGEWND
   {
      RETURN( FALSE);
   }

   _SEH2_TRY
   {
      ProbeForRead(lpwndpl, sizeof(WINDOWPLACEMENT), 1);
      RtlCopyMemory(&Safepl, lpwndpl, sizeof(WINDOWPLACEMENT));
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      SetLastNtError(_SEH2_GetExceptionCode());
      _SEH2_YIELD(RETURN( FALSE));
   }
   _SEH2_END

   if(Safepl.length != sizeof(WINDOWPLACEMENT))
   {
      RETURN( FALSE);
   }

   Flags = PLACE_MAX | PLACE_RECT;
   if (Safepl.flags & WPF_SETMINPOSITION) Flags |= PLACE_MIN;
   UserRefObjectCo(Wnd, &Ref);
   IntSetWindowPlacement(Wnd, &Safepl, Flags);
   UserDerefObjectCo(Wnd);
   RETURN(TRUE);

CLEANUP:
   TRACE("Leave NtUserSetWindowPlacement, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserShowWindowAsync(HWND hWnd, LONG nCmdShow)
{
   PWND Window;
   BOOL ret;
   DECLARE_RETURN(BOOL);
   USER_REFERENCE_ENTRY Ref;

   TRACE("Enter NtUserShowWindowAsync\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)) || // FIXME:
         Window == UserGetDesktopWindow() ||    // pWnd->fnid == FNID_DESKTOP
         Window == UserGetMessageWindow() )     // pWnd->fnid == FNID_MESSAGEWND
   {
      RETURN(FALSE);
   }

   if ( nCmdShow > SW_MAX )
   {
      EngSetLastError(ERROR_INVALID_PARAMETER);
      RETURN(FALSE);
   }

   UserRefObjectCo(Window, &Ref);
   ret = co_IntSendMessageNoWait( hWnd, WM_ASYNC_SHOWWINDOW, nCmdShow, 0 );
   UserDerefObjectCo(Window);
   if (-1 == (int) ret || !ret) ret = FALSE;

   RETURN(ret);

CLEANUP:
   TRACE("Leave NtUserShowWindowAsync, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/*
 * @implemented
 */
BOOL APIENTRY
NtUserShowWindow(HWND hWnd, LONG nCmdShow)
{
   PWND Window;
   BOOL ret;
   DECLARE_RETURN(BOOL);
   USER_REFERENCE_ENTRY Ref;

   TRACE("Enter NtUserShowWindow\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)) || // FIXME:
         Window == UserGetDesktopWindow() ||    // pWnd->fnid == FNID_DESKTOP
         Window == UserGetMessageWindow() )     // pWnd->fnid == FNID_MESSAGEWND
   {
      RETURN(FALSE);
   }

   if ( nCmdShow > SW_MAX || Window->state2 & WNDS2_INDESTROY)
   {
      EngSetLastError(ERROR_INVALID_PARAMETER);
      RETURN(FALSE);
   }

   UserRefObjectCo(Window, &Ref);
   ret = co_WinPosShowWindow(Window, nCmdShow);
   UserDerefObjectCo(Window);

   RETURN(ret);

CLEANUP:
   TRACE("Leave NtUserShowWindow, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}


/*
 *    @implemented
 */
HWND APIENTRY
NtUserWindowFromPoint(LONG X, LONG Y)
{
   POINT pt;
   HWND Ret;
   PWND DesktopWindow = NULL, Window = NULL;
   USHORT hittest;
   DECLARE_RETURN(HWND);
   USER_REFERENCE_ENTRY Ref;

   TRACE("Enter NtUserWindowFromPoint\n");
   UserEnterExclusive();

   if ((DesktopWindow = UserGetWindowObject(IntGetDesktopWindow())))
   {
      //PTHREADINFO pti;

      pt.x = X;
      pt.y = Y;

      // Hmm... Threads live on desktops thus we have a reference on the desktop and indirectly the desktop window.
      // It is possible this referencing is useless, though it should not hurt...
      UserRefObjectCo(DesktopWindow, &Ref);

      //pti = PsGetCurrentThreadWin32Thread();
      Window = co_WinPosWindowFromPoint(DesktopWindow, &pt, &hittest);

      if (Window)
      {
         Ret = UserHMGetHandle(Window);

         RETURN( Ret);
      }
   }

   RETURN( NULL);

CLEANUP:
   if (Window) UserDereferenceObject(Window);
   if (DesktopWindow) UserDerefObjectCo(DesktopWindow);

   TRACE("Leave NtUserWindowFromPoint, ret=%i\n",_ret_);
   UserLeave();
   END_CLEANUP;
}

/* EOF */
