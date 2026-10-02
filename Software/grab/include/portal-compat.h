/*
 * portal-compat.h
 *
 *  Compatibility shim for xdg-desktop-portal / glib headers.
 *
 *  glib-object.h (pulled in by portal.h → gio/gio.h) defines 'public'
 *  as a macro for GObject introspection. This conflicts with C++ 'public'
 *  keyword used by Qt's Q_OBJECT / Q_SIGNALS macros.
 *
 *  Solution: undefine 'public' before including any Qt headers.
 *  Include this file FIRST, before portal.h and Qt headers.
 *
 *  Copyright (c) 2026 Lightpack contributors
 */

#pragma once

#ifdef public
#undef public
#endif
