//
// Created by yang on 2024/4/18.
//

#ifndef TERMUX_X11_WINDOW_MANAGER_H
#define TERMUX_X11_WINDOW_MANAGER_H
#include "X11/Xlib.h"
#include "X11/X.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "util.hpp"
#include <android/log.h>
#define log(...) __android_log_print(ANDROID_LOG_DEBUG, "huyang_wm", __VA_ARGS__)
#define CHECK(condition)  \
      if(condition){     \
          log("#condition fatal");                \
      }
#define CHECK_EQ(val1, val2)  \
      if(val1 != val2){     \
          log("not equal");                \
      }
#define HAVE_COMPOSITOR 1



class WindowManager  {
public:
    static ::WindowManager* create();
    ~WindowManager();
    WindowManager(Display *display);
    void Run();
    int moveWindow(long ptr, int x, int y);
    int resizeWindow(long ptr, int x, int y);
    int closeWindow(long ptr);
    int raiseWindow(long ptr);
    void initCompositor();
    int stoped = False;

private:

    // Handle to the underlying Xlib Display struct.
    Display* display_;
    // Handle to root window.
    const Window root_;
    // Frames a top-level window.
    void Frame(Window w, bool was_created_before_window_manager);
    // Unframes a client window.
    void Unframe(Window w);

    // Event handlers.
    void OnCreateNotify(const XCreateWindowEvent& e);
    void OnDestroyNotify(const XDestroyWindowEvent& e);
    void OnReparentNotify(const XReparentEvent& e);
    void OnMapNotify(const XMapEvent& e);
    void OnUnmapNotify(const XUnmapEvent& e);
    void OnConfigureNotify(const XConfigureEvent& e);
    void OnMapRequest(const XMapRequestEvent& e);
    void OnConfigureRequest(const XConfigureRequestEvent& e);
    void OnButtonPress(const XButtonEvent& e);
    void OnButtonRelease(const XButtonEvent& e);
    void OnMotionNotify(const XMotionEvent& e);
    void OnKeyPress(const XKeyEvent& e);
    void OnKeyRelease(const XKeyEvent& e);

    // Xlib error handler. It must be static as its address is passed to Xlib.
    static int OnXError(Display* display, XErrorEvent* e);
    // Xlib error handler used to determine whether another window manager is
    // running. It is set as the error handler right before selecting substructure
    // redirection mask on the root window, so it is invoked if and only if
    // another window manager is running. It must be static as its address is
    // passed to Xlib.
    static int OnWMDetected(Display* display, XErrorEvent* e);
    // Whether an existing window manager has been detected. Set by OnWMDetected,
    // and hence must be static.
    static bool wm_detected_;
    // A mutex for protecting wm_detected_. It's not strictly speaking needed as
    // this program is single threaded, but better safe than sorry.
    static ::std::mutex wm_detected_mutex_;


    // Maps top-level windows to their frame windows.
    ::std::unordered_map<Window, Window> clients_;

    // The cursor position at the start of a window move/resize.
    Position<int> drag_start_pos_;
    // The position of the affected window at the start of a window
    // move/resize.
    Position<int> drag_start_frame_pos_;
    // The size of the affected window at the start of a window move/resize.
    Size<int> drag_start_frame_size_;

    // Atom constants.
    const Atom WM_PROTOCOLS;
    const Atom WM_DELETE_WINDOW;
};


#endif //TERMUX_X11_WINDOW_MANAGER_H