#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <getopt.h>
#include <glib.h>
#include <pthread.h>
#include <regex.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlibint.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/record.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <X11/keysymdef.h>
#include <xfconf/xfconf.h>

#define WHISKER "/usr/bin/xfce4-popup-whiskermenu"
#define APPLICATIONS "/usr/bin/xfce4-popup-applicationsmenu"
#define FINDER "xfce4-appfinder &"
#define QUEUE_SIZE 5

/* gcc -o whisker_launcher whisker_launcher.c `pkg-config --cflags libxfconf-0` -lX11 -lXtst -lXi  -lxfconf-0 -lglib-2.0 -pthread */

typedef struct {
    void (*function)(void*);
    void *argument;
} task_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    task_t task_queue[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    int shutdown;
} single_thread_pool_t;

/* for this struct, refer to libxnee */
typedef union {
    unsigned char    type ;
    xEvent           event ;
    xResourceReq     req   ;
    xGenericReply    reply ;
    xError           error ;
    xConnSetupPrefix setup;
} XRecordDatum;

// Data structure to track key presses
typedef struct {
    int key_code;
    int pressed; // 1 if key is pressed, 0 if released
} KeyState;

pthread_mutex_t lock;
single_thread_pool_t pool;

KeyState super_l_state = {0, 0};
KeyState super_r_state = {0, 0};

// Global variables for displays and XRecordContext
Display *d = NULL;
Display *d_control = NULL;
XRecordContext context = 0;
char *menu = NULL;
static int super_l_pressed = 0;
static int super_r_pressed = 0;
static int keep_runing = 1;

/* Function declarations */
void      log_message                       (const char            *message);
int       check_window_property             (Window                window,
                                             Atom                  property,
                                             const char            *value);
Window    find_window_recursive             (Window                window,
                                             Atom                  wm_class,
                                             Atom                  wm_icon_name,
                                             Atom                  net_wm_icon_name,
                                             Atom                  wm_name,
                                             Atom                  net_wm_name);
Window    find_window                       (void);
int       is_window_hidden                  (Window                window);
void      run_menu_and_wait                 (void);
gboolean  is_plugin_key                     (const char            *key);
void      send_key                          (KeySym                keysym);
char      *find_menus                       (void);
static int window_exists_error_handler      (Display               *display,
                                             XErrorEvent           *error);
int       window_exists                     (Display               *display,
                                             Window                window);
void      manage_menu                       (void                  *data);
void      event_callback                    (XPointer              priv,
                                             XRecordInterceptData  *hook);
void      clean                             (void);
XRecordRange*    create_record_range        (void);
void      setup_record_context              (void);
void*     thread_worker                     (void *arg);
void      single_thread_pool_init           (void);
void      single_thread_pool_add_task       (void (*function)(void*),
                                             void *argument);
void      single_thread_pool_destroy        (void);
Window    get_focused_window                (void);
int       is_focused_window_app_finder      (Window                app_finder_window);

void* thread_worker(void *arg) {
    while (1) {
        pthread_mutex_lock(&pool.lock);
        while (pool.count == 0 && !pool.shutdown) {
            pthread_cond_wait(&pool.cond, &pool.lock);
        }
        if (pool.shutdown) {
            pthread_mutex_unlock(&pool.lock);
            pthread_exit(NULL);
        }
        task_t task = pool.task_queue[pool.head];
        pool.head = (pool.head + 1) % QUEUE_SIZE;
        pool.count--;
        pthread_mutex_unlock(&pool.lock);

        if (task.function != NULL) {
            task.function(task.argument);
        }
    }
}

void single_thread_pool_init() {
    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.cond, NULL);
    pool.head = 0;
    pool.tail = 0;
    pool.count = 0;
    pool.shutdown = 0;
    pthread_create(&pool.thread, NULL, thread_worker, NULL);
    printf("Single-thread pool initialized.\n");
}

void single_thread_pool_add_task(void (*function)(void*), void *argument) {
  /*  if (pthread_mutex_trylock(&lock) == 0)
     {*/
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(&pool.lock);
        if (pool.count < QUEUE_SIZE) {
           pool.task_queue[pool.tail].function = function;
           pool.task_queue[pool.tail].argument = argument;
           pool.tail = (pool.tail + 1) % QUEUE_SIZE;
           pool.count++;
           pthread_cond_signal(&pool.cond);
        } else {
           printf("Task queue is full. Task not added.\n");
        }
        pthread_mutex_unlock(&pool.lock);
  /* } else {
        printf("Task is already running. Task not added.\n");
     }*/
}

void single_thread_pool_destroy() {
    pthread_mutex_lock(&pool.lock);
    pool.shutdown = 1;
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.lock);
    pthread_join(pool.thread, NULL);
    pthread_mutex_destroy(&pool.lock);
    pthread_cond_destroy(&pool.cond);
    printf("Single-thread pool destroyed.\n");
}

// Function to log messages to stderr
void log_message(const char *message) {
    fprintf(stderr, "%s\n", message);
}

// Function to check if a window has the specified property with a specific value
int check_window_property(Window window, Atom property, const char *value) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop;
    int result = 0;

    if (window_exists(d, window) && XGetWindowProperty(d, window, property, 0, 1024, False, AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        if (actual_type != None) {
         if (prop) {
            if (strcmp((char *)prop, value) == 0) {
                result = 1;
            }
            XFree(prop);
        }
       }
    }
    return result;
}

// Function to recursively search for the Menu window in the window's children
Window find_window_recursive(Window window, Atom wm_class, Atom wm_icon_name, Atom net_wm_icon_name, Atom wm_name, Atom net_wm_name) {
    // Check if the current window is the Menu window
    if (strcmp(menu, "whiskermenu") == 0) {
        if (check_window_property(window, wm_class, "wrapper-2.0") &&
            check_window_property(window, wm_icon_name, "Whisker Menu") &&
            check_window_property(window, net_wm_icon_name, "Whisker Menu") &&
            check_window_property(window, wm_name, "Whisker Menu") &&
            check_window_property(window, net_wm_name, "Whisker Menu")) {
            return window;  // Found the Whisker Menu window
        }
    } else if (strcmp(menu, "finder") == 0) {
        if (check_window_property(window, wm_class, "xfce4-appfinder") &&
            check_window_property(window, wm_icon_name, "Application Finder") &&
            check_window_property(window, net_wm_icon_name, "Application Finder") &&
            check_window_property(window, wm_name, "Application Finder") &&
            check_window_property(window, net_wm_name, "Application Finder")) {
            return window;  // Found the Application Finder Menu window
        }
    } else if (strcmp(menu, "applicationsmenu") == 0) {
        if ((check_window_property(window, wm_class, "xfce4-panel") == 0) &&
            check_window_property(window, wm_icon_name, "xfce4-panel") &&
            check_window_property(window, net_wm_icon_name, "xfce4-panel") &&
            check_window_property(window, wm_name, "xfce4-panel") &&
            check_window_property(window, net_wm_name, "xfce4-panel")) {
            return window;  // Found the Applications Menu window
        }
    }

    // Recursively search for the Menu window in the window's children
    Window parent;
    Window *children = NULL;
    unsigned int nchildren;
    if (window_exists(d, window) && XQueryTree(d, window, &window, &parent, &children, &nchildren) != 0) {
        for (unsigned int i = 0; i < nchildren; i++) {
            Window menu_id = find_window_recursive(children[i], wm_class, wm_icon_name, net_wm_icon_name, wm_name, net_wm_name);
            if (menu_id != 0) {
                XFree(children);  // Free the memory allocated by XQueryTree
                return menu_id;  // Found the Whisker Menu window
            }
        }
        XFree(children);  // Free the memory allocated by XQueryTree
    }

    return 0;  // Not found
}

// Function to find the Whisker Menu window
Window find_window() {
    Window root = DefaultRootWindow(d);
    Window parent;
    Window *children = NULL;
    unsigned int nchildren;
    Atom wm_class = XInternAtom(d, "WM_CLASS", False);
    Atom wm_icon_name = XInternAtom(d, "WM_ICON_NAME", False);
    Atom net_wm_icon_name = XInternAtom(d, "_NET_WM_ICON_NAME", False);
    Atom wm_name = XInternAtom(d, "WM_NAME", False);
    Atom net_wm_name = XInternAtom(d, "_NET_WM_NAME", False);

    // Query the tree to get all windows
    if (XQueryTree(d, root, &root, &parent, &children, &nchildren) != 0) {
        // Recursively search for the Menu window
        Window menu_id = 0;
        for (unsigned int i = 0; i < nchildren; i++) {
            menu_id = find_window_recursive(children[i], wm_class, wm_icon_name, net_wm_icon_name, wm_name, net_wm_name);
            if (menu_id != 0) {
                break;  // Found the Whisker Menu window, exit loop
            }
        }
        XFree(children);  // Free the memory allocated by XQueryTree
        return menu_id;
    } else {
        fprintf(stderr, "XQueryTree failed\n");
        return 0;
    }
}

// Function to check if the window is hidden
int is_window_hidden(Window window) {
    Atom wm_state_hidden = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    Atom wm_state = XInternAtom(d, "WM_STATE", False);

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop;
    int is_hidden = 0;

    if (window != 0 && window_exists(d, window) && XGetWindowProperty(d, window, wm_state, 0, 1, False, wm_state,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        if (nitems > 0 && actual_format == 32 && prop != NULL) {
            long state = *(long *)prop;
            is_hidden = (state == WithdrawnState);
        }
        XFree(prop);
    }
    return is_hidden;
}

// Function to run xfce4-popup-whiskermenu and wait for it to show up
void run_menu_and_wait() {
    int max_attempts = 20;
    int sleep_duration = 10000; // 10 milliseconds

    if (menu == "whiskermenu") {
        system(WHISKER);
    } else if (menu == "applicationsmenu") {
        system(APPLICATIONS); 
    } else {
        system(FINDER);
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        usleep(sleep_duration);
        Window menu_id = find_window();
        if (menu_id != 0 && !is_window_hidden(menu_id)) {
            //printf("0x%8x\n", menu_id);
            log_message("Menu is now visible");
            return;
        }
    }
    if (menu == "whiskermenu") {
        system(WHISKER);
    }
}

/* function to simulate both the key press and release events */
void send_key(KeySym keysym) {
    KeyCode keycode = XKeysymToKeycode(d, keysym);
    
    // Simulate key press
    XTestFakeKeyEvent(d, keycode, True, CurrentTime);
    XFlush(d);

    
    // Simulate key release
    XTestFakeKeyEvent(d, keycode, False, CurrentTime);
    XFlush(d);

}

// Function to check if the key matches the pattern "plugin-<number>"
gboolean is_plugin_key(const char *key) {
    regex_t regex;
    int ret;

    // Compile the regular expression
    ret = regcomp(&regex, "^/plugins/plugin-[0-9]+$", REG_EXTENDED);
    if (ret) {
        fprintf(stderr, "Could not compile regex\n");
        return FALSE;
    }

    // Execute the regular expression
    ret = regexec(&regex, key, 0, NULL, 0);
    regfree(&regex);

    return ret == 0;
}

// Funtion to check if whisker and applications menu are present in the panel
char *find_menus () {
    GError *error = NULL;
    GHashTable *properties = NULL;
    GHashTableIter iter;
    gboolean whisker_menu_present = FALSE;
    gboolean applications_menu_present = FALSE;
    gpointer key, value;

    // Initialize xfconf
    if (!xfconf_init(&error)) {
        fprintf(stderr, "Failed to initialize xfconf: %s\n", error->message);
        g_error_free(error);
        return NULL;
    }

    // Open the xfce4-panel channel
    XfconfChannel *channel = xfconf_channel_get("xfce4-panel");
    if (!channel) {
        fprintf(stderr, "Failed to open xfce4-panel channel\n");
        return NULL;
    }

    // Get the list of properties under /plugins
    properties = xfconf_channel_get_properties(channel, "/plugins");
    if (!properties) {
        fprintf(stderr, "Error getting properties or no properties found\n");
        return NULL;
    }

    // Iterate over the properties to check for plugin presence
    g_hash_table_iter_init(&iter, properties);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (is_plugin_key((char *)key)) {
            gchar *v = xfconf_channel_get_string(channel, key, NULL);
            if (v) {
                // Check for Whisker Menu
                if (g_strrstr(v, "whiskermenu") != NULL) {
                    whisker_menu_present = TRUE;
                }
                // Check for Applications Menu
                if (g_strrstr(v, "applicationsmenu") != NULL) {
                    applications_menu_present = TRUE;
                }
                g_free(v);
            }
        }
    }

    xfconf_shutdown();

    // Clean up
    if (properties) {
        g_hash_table_unref(properties);
    }

    if (whisker_menu_present) {
        return "whiskermenu";
    } else if (applications_menu_present) {
        return "applicationsmenu";
    } else {
        return NULL;
    }
}

// Custom error handler to catch BadWindow errors
static int window_exists_error_handler(Display *display, XErrorEvent *error) {
    if (error->error_code == BadWindow) {
        return 0; // Indicates that the window does not exist
    }
    return 1; // Indicates an error other than BadWindow occurred
}

// Function to check if a window exists
int window_exists(Display *display, Window window) {
    XWindowAttributes attributes;
    int (*old_handler)(Display *, XErrorEvent *);

    // Set the custom error handler
    old_handler = XSetErrorHandler(window_exists_error_handler);

    // Try to get the window attributes
    if (XGetWindowAttributes(display, window, &attributes)) {
        // Restore the old error handler
        XSetErrorHandler(old_handler);
        return 1; // The window exists
    }

    // Restore the old error handler
    XSetErrorHandler(old_handler);
    return 0; // The window does not exist
}

/* Manage Menu window */
void manage_menu(void *data) {
    // Find the Menu in the xfce4-panel
    pthread_mutex_lock(&lock);
    menu = find_menus();
    if (menu == NULL) {
        menu = "finder";
    }

    // Find the Menu window
    Window menu_id = find_window();
    if (menu_id != 0) {
        if (is_window_hidden(menu_id)) {
            run_menu_and_wait();
        } else {
            if (strcmp(menu, "finder") == 0) {
                XEvent event;
                event.xclient.type = ClientMessage;
                event.xclient.window = menu_id;
                event.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", TRUE);
                event.xclient.format = 32;
                event.xclient.data.l[0] = XInternAtom(d, "WM_DELETE_WINDOW", FALSE);
                event.xclient.data.l[1] = CurrentTime;
                XSendEvent(d, menu_id, False, NoEventMask, &event);  // soft close appfinder
                XFlush(d);
                    int max_attempts = 20;
                    int sleep_duration = 10000; // 10 milliseconds
                    for (int attempt = 0; attempt < max_attempts; attempt++) {
                        usleep(sleep_duration);
                        if (window_exists(d, menu_id)) {
                            //printf("0x%lx\n", menu_id);
                            break;
                        }
                   }
                   log_message("Application Finder is now hidden");
            } else if (strcmp(menu, "whiskermenu") == 0) {
                    // Send escape key
                    send_key(XK_Escape);
                    int max_attempts = 20;
                    int sleep_duration = 10000; // 10 milliseconds
                    for (int attempt = 0; attempt < max_attempts; attempt++) {
                        usleep(sleep_duration);
                        if (is_window_hidden(menu_id)) {
                            //printf("0x%lx\n", menu_id);
                            break;
                        }
                   }
                   log_message("Whisker Menu is now hidden");
            } else {
                // Send escape key
                    send_key(XK_Escape);
                    int max_attempts = 20;
                    int sleep_duration = 10000; // 10 milliseconds
                    for (int attempt = 0; attempt < max_attempts; attempt++) {
                        usleep(sleep_duration);
                        if (is_window_hidden(menu_id)) {
                            //printf("0x%lx\n", menu_id);
                            break;
                        }
                   }
                   log_message("Applications Menu is now hidden");
            }
        }
    } else {
        // Menu window ID not found, launching Menu...
        run_menu_and_wait();
    }
    menu = 0;
    pthread_mutex_unlock(&lock);
}

/* Callback function for XRecordContext */
void event_callback(XPointer priv, XRecordInterceptData *hook) {
    /* log_message("Event callback triggered"); */

    if (hook->category != XRecordFromServer) {
        XRecordFreeData(hook);
        return;
    }

    XRecordDatum *data = (XRecordDatum*) hook->data;
    int event_type = data->type;
    BYTE keycode = data->event.u.u.detail;

    if (event_type == KeyPress || event_type == KeyRelease) {
        KeySym keysym = XkbKeycodeToKeysym(d, keycode, 0, 0);

        /*char buffer[256];
        snprintf(buffer, sizeof(buffer), "Key event: type=%d, keycode=%u, keysym=%lu",
                 data->event.u.u.type, keycode, keysym);
        log_message(buffer); */

        if (keysym == XK_Super_L) {
            if (event_type == KeyPress) {
                super_l_state.key_code = keycode;
                super_l_state.pressed = 1;
            } else if (event_type == KeyRelease) {
                if (super_l_state.pressed && super_l_state.key_code == keycode) {
                    //log_message("Super_L key released (after press)");
                    single_thread_pool_add_task(manage_menu, NULL);
                }
            }
        } else if (keysym == XK_Super_R) {
            if (event_type == KeyPress) {
                super_r_state.key_code = keycode;
                super_r_state.pressed = 1;
            } else if (event_type == KeyRelease) {
                if (super_r_state.pressed && super_r_state.key_code == keycode) {
                    //log_message("Super_R key released (after press)");
                    single_thread_pool_add_task(manage_menu, NULL);
                }
            }
        } else {
            //printf("Other key pressed\n");
            super_l_state.pressed = 0;
            super_r_state.pressed = 0;
            super_l_state.key_code = keycode;
            super_r_state.key_code = keycode;

        }
    } /*else {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Non-key event: type=%d", data->type);
        log_message(buffer);
    }*/

    XRecordFreeData(hook);
}

void cleanup(void) {
    log_message("Cleaning up resources...");
    if (context) {
        XRecordDisableContext(d_control, context);
        XRecordFreeContext(d_control, context);
        context = 0;
    }
    log_message("D_control...");
    if (d_control) {
        XCloseDisplay(d_control);
        d_control = NULL;
    }
    if (d) {
        XCloseDisplay(d);
        d = NULL;
    }
    if (menu) {
        free(menu);
    }
    single_thread_pool_destroy();
    pthread_mutex_destroy(&lock);
    log_message("Done...");
}

/* Create an XRecordRange for recording key events */
XRecordRange* create_record_range(void) {
    XRecordRange *range = XRecordAllocRange();
    if (!range) {
        log_message("Unable to allocate XRecordRange");
        cleanup();
        exit(1);
    }
    range->device_events.first = KeyPress;
    range->device_events.last = KeyRelease;
    return range;
}

/* Set up the XRecord context for capturing key events */
void setup_record_context(void) {
    XRecordClientSpec clients = XRecordAllClients;
    XRecordRange *range = create_record_range();
    context = XRecordCreateContext(d_control, 0, &clients, 1, &range, 1);

    if (!context) {
        log_message("Unable to create XRecordContext");
        cleanup();
        exit(1);
    } else {
        log_message("XRecordContext created successfully");
    }

    if (!XRecordEnableContextAsync(d_control, context, event_callback, NULL)) {
        log_message("Unable to enable XRecordContext");
        cleanup();
        exit(1);
    } else {
        log_message("XRecordContext enabled successfully");
    }

    XFree(range);
}

/* Main function */
int main(int argc, char **argv) {
    d = XOpenDisplay(NULL);
    if (d == NULL) {
        log_message("Cannot open display");
        exit(1);
    }

    d_control = XOpenDisplay(NULL);
    if (d_control == NULL) {
        log_message("Cannot open control display");
        exit(1);
    }

    XSynchronize(d, True); // Ensure synchronized operation on display
    XSynchronize(d_control, True); // Ensure synchronized operation on control display

    setup_record_context();

    single_thread_pool_init();
    pthread_mutex_init(&lock, NULL);

    while (keep_runing) {
        if (XPending(d_control)) {
            XRecordProcessReplies(d_control);
        }
        usleep(100000);  // Sleep for 100 milliseconds to reduce CPU usage
    }

    single_thread_pool_destroy();
    pthread_mutex_destroy(&lock);

    if (menu) {
        menu = NULL;
        free(menu);
    }

    XFlush(d);
    XSync(d, True);

    if (context) {
        XRecordDisableContext(d_control, context);
        XRecordFreeContext(d_control, context);
        context = 0;
    }
    if (d_control) {
        XCloseDisplay(d_control);
        d_control = NULL;
    }
    if (d) {
        XCloseDisplay(d);
        d = NULL;
    }

    return 0;
}