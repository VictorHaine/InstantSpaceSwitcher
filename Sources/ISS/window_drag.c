#include "window_drag.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "window_focus.h"

// Window position and cursor are sampled at slightly different times, so a fast drag shows
// some lag. Text selection and held clicks never move the window and resizes change its
// size, so a generous tolerance does not let those through.
static const double kDragTolerance = 32.0;
// How far the cursor may wander while a window is held still on its title bar
static const double kHoldSlop = 4.0;

static CFMachPortRef mouseTap = NULL;
static CFRunLoopSourceRef mouseSource = NULL;

// Snapshot taken at left mouse down
static bool candidateValid = false;
static uint32_t candidateWindow = 0;
static CGRect candidateFrame;
static CGPoint candidateCursor;

void iss_carry_log(const char *format, ...) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *value = getenv("ISS_DEBUG_CARRY");
        enabled = value && strcmp(value, "1") == 0;
    }
    if (!enabled) {
        return;
    }
    va_list args;
    va_start(args, format);
    fprintf(stderr, "ISS carry: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

bool iss_window_is_held(CGRect downFrame, CGPoint downCursor,
                        CGRect frame, CGPoint cursor, double slop) {
    return frame.size.width == downFrame.size.width && frame.size.height == downFrame.size.height &&
           fabs(frame.origin.x - downFrame.origin.x) < 1.0 && fabs(frame.origin.y - downFrame.origin.y) < 1.0 &&
           fabs(cursor.x - downCursor.x) <= slop && fabs(cursor.y - downCursor.y) <= slop;
}

bool iss_window_follows_cursor(CGRect downFrame, CGPoint downCursor,
                               CGRect frame, CGPoint cursor, double tolerance) {
    if (frame.size.width != downFrame.size.width || frame.size.height != downFrame.size.height) {
        return false; // resizing, not moving
    }
    const double windowDX = frame.origin.x - downFrame.origin.x;
    const double windowDY = frame.origin.y - downFrame.origin.y;
    if (fabs(windowDX) < 1.0 && fabs(windowDY) < 1.0) {
        return false; // held click or text selection: the window never moved
    }
    const double cursorDX = cursor.x - downCursor.x;
    const double cursorDY = cursor.y - downCursor.y;
    return fabs(windowDX - cursorDX) <= tolerance && fabs(windowDY - cursorDY) <= tolerance;
}

bool iss_window_covers_display(CGRect windowBounds, CGRect displayBounds) {
    // A couple of points of slack: scaled or external displays can round the two rects differently
    return !CGRectIsEmpty(displayBounds) && CGRectContainsRect(CGRectInset(windowBounds, -2.0, -2.0), displayBounds);
}

static bool window_bounds(CFDictionaryRef info, CGRect *outBounds) {
    CFDictionaryRef bounds = (CFDictionaryRef)CFDictionaryGetValue(info, kCGWindowBounds);
    return bounds && CGRectMakeWithDictionaryRepresentation(bounds, outBounds);
}

static int window_int(CFDictionaryRef info, CFStringRef key, int fallback) {
    CFNumberRef number = (CFNumberRef)CFDictionaryGetValue(info, key);
    int value = fallback;
    if (number) {
        CFNumberGetValue(number, kCFNumberIntType, &value);
    }
    return value;
}

// Records the frontmost visible window under the point, if it is a normal (layer 0) window
// of another process. Anything else on top (menus, panels, the Dock) means no candidate.
static void record_candidate(CGPoint point) {
    candidateValid = false;
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!windows) {
        return;
    }
    const pid_t ownPid = getpid();
    CGDirectDisplayID displayID = 0;
    uint32_t displayCount = 0;
    CGRect displayBounds = CGRectZero;
    if (CGGetDisplaysWithPoint(point, 1, &displayID, &displayCount) == kCGErrorSuccess && displayCount > 0) {
        displayBounds = CGDisplayBounds(displayID);
    }
    for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(windows, i);
        CGRect bounds;
        if (!window_bounds(info, &bounds) || !CGRectContainsPoint(bounds, point)) {
            continue;
        }
        CFNumberRef alpha = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowAlpha);
        double alphaValue = 1.0;
        if (alpha) {
            CFNumberGetValue(alpha, kCFNumberDoubleType, &alphaValue);
        }
        if (alphaValue <= 0.0 || window_int(info, kCGWindowOwnerPID, 0) == ownPid) {
            continue; // invisible overlays and our own OSD
        }
        const int layer = window_int(info, kCGWindowLayer, -1);
        if (layer != 0 && iss_window_covers_display(bounds, displayBounds)) {
            continue; // full-screen system overlay (Notification Center, Dock): click passes through
        }
        if (layer == 0) {
            candidateWindow = (uint32_t)window_int(info, kCGWindowNumber, 0);
            candidateFrame = bounds;
            candidateCursor = point;
            candidateValid = candidateWindow != 0;
        } else {
            char owner[64] = "?";
            CFStringRef name = (CFStringRef)CFDictionaryGetValue(info, kCGWindowOwnerName);
            if (name) {
                CFStringGetCString(name, owner, sizeof(owner), kCFStringEncodingUTF8);
            }
            iss_carry_log("mouse down at (%.0f, %.0f) blocked by layer %d window of %s, bounds (%.0f, %.0f, %.0f x %.0f), "
                          "display (%.0f, %.0f, %.0f x %.0f)", point.x, point.y, layer, owner,
                          bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height,
                          displayBounds.origin.x, displayBounds.origin.y,
                          displayBounds.size.width, displayBounds.size.height);
        }
        break;
    }
    CFRelease(windows);
}

static CGEventRef mouseTapCallback(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy;
    (void)refcon;
    switch (type) {
    case kCGEventTapDisabledByTimeout:
    case kCGEventTapDisabledByUserInput:
        if (mouseTap) {
            CGEventTapEnable(mouseTap, true);
        }
        candidateValid = false;
        break;
    case kCGEventLeftMouseDown:
        record_candidate(CGEventGetLocation(event));
        break;
    case kCGEventLeftMouseUp:
        candidateValid = false;
        break;
    default:
        break;
    }
    return event;
}

bool iss_drag_tracker_start(void) {
    if (mouseTap) {
        return true;
    }
    CGEventMask mask = CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp);
    // An active tap (the callback passes every event through untouched) needs only the Accessibility
    // permission; a listen-only tap also needs Input Monitoring, which a fresh Mac has not granted,
    // so carrying a dragged window silently never worked there.
    mouseTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionDefault,
                                mask, mouseTapCallback, NULL);
    if (!mouseTap) {
        return false;
    }
    mouseSource = CFMachPortCreateRunLoopSource(NULL, mouseTap, 0);
    if (!mouseSource) {
        CFRelease(mouseTap);
        mouseTap = NULL;
        return false;
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), mouseSource, kCFRunLoopCommonModes);
    CGEventTapEnable(mouseTap, true);
    return true;
}

void iss_drag_tracker_stop(void) {
    candidateValid = false;
    if (!mouseTap) {
        return;
    }
    CGEventTapEnable(mouseTap, false);
    if (mouseSource) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), mouseSource, kCFRunLoopCommonModes);
        CFRelease(mouseSource);
        mouseSource = NULL;
    }
    CFRelease(mouseTap);
    mouseTap = NULL;
}

bool iss_drag_current_window(uint32_t *outWindowID) {
    if (!candidateValid || !outWindowID) {
        iss_carry_log("no normal window was under the cursor at mouse down");
        return false;
    }
    // The press is tracked from the mouse events themselves (candidateValid is set on mouse down
    // and cleared on mouse up or when the tap is disabled). The hardware button state is not
    // asked: trackpad tap-dragging, drag lock and three-finger drag press the button in software,
    // so it reads as up while a window is being dragged.
    iss_carry_log("button state: tracked down, hardware %s",
                  CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft)
                      ? "down" : "up");

    CFArrayRef info = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, candidateWindow);
    CGRect frame;
    bool found = info && CFArrayGetCount(info) > 0 &&
                 window_bounds((CFDictionaryRef)CFArrayGetValueAtIndex(info, 0), &frame);
    if (info) {
        CFRelease(info);
    }
    if (!found) {
        iss_carry_log("window %u disappeared", candidateWindow);
        return false;
    }

    CGEventRef now = CGEventCreate(NULL);
    if (!now) {
        return false;
    }
    CGPoint cursor = CGEventGetLocation(now);
    CFRelease(now);

    // Holding the title bar without moving yet grabs the window too, as it does natively
    if (iss_window_is_held(candidateFrame, candidateCursor, frame, cursor, kHoldSlop) &&
        iss_point_is_window_drag_area(candidateCursor, candidateWindow)) {
        *outWindowID = candidateWindow;
        return true;
    }

    if (!iss_window_follows_cursor(candidateFrame, candidateCursor, frame, cursor, kDragTolerance)) {
        iss_carry_log("window %u not following the cursor: window moved (%.0f, %.0f), cursor (%.0f, %.0f), "
                      "size %.0fx%.0f -> %.0fx%.0f", candidateWindow,
                      frame.origin.x - candidateFrame.origin.x, frame.origin.y - candidateFrame.origin.y,
                      cursor.x - candidateCursor.x, cursor.y - candidateCursor.y,
                      candidateFrame.size.width, candidateFrame.size.height, frame.size.width, frame.size.height);
        return false;
    }
    *outWindowID = candidateWindow;
    return true;
}
