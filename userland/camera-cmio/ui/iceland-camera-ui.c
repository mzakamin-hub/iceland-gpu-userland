// Ours (MIT). iceland-camera-ui: the three bits of UI that an unattended boot of our test install needs moved, through
// the Accessibility API, from a LaunchAgent job of the logged-in user: our camera extension switched on in System
// Settings, a camera-access alert accepted, Photo Booth's shutter pressed. Public APIs only: ApplicationServices'
// AXUIElement calls for the trees and the presses, CoreFoundation for the values, libproc to find a process by name. No
// AppKit and no Objective-C: everything here is C.
//
//   iceland-camera-ui enable-extension NEEDLE [SECONDS]            find NEEDLE in System Settings' windows and switch
//                                                                  on the switch of its row (or press the row's (i)
//                                                                  button and wait for the sheet's switch), then watch
//                                                                  20 s for the SecurityAgent alert the change brings
//   iceland-camera-ui press-button APP TITLE[,TITLE...] [SECONDS]   press the button titled TITLE in APP ("any": the
//                                                                  system's alert and settings windows)
//   iceland-camera-ui shoot [SECONDS]                              press Photo Booth's shutter
//   iceland-camera-ui dump APP [MAX]                               an app's windows as "ax" lines
//
// Every line goes to stderr as "camera-ui: ...": the kernel copies the stderr of a process named iceland-camera-* into
// the boot log, which is all a tablet boot keeps, so the lines say what was found and what was pressed, compactly.
// Exit 0: done (also when the switch was on already, or when no alert came); 1: it gave up; 2: bad arguments.
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    kFieldChars = 60,                   // a field of an "ax" line
    kFieldSize = 64,                    // its buffer (60 characters and a NUL, with room for "..")
    kMaxDepth = 30,                     // how deep one walk goes
    kMaxNodes = 4000,                   // how many elements one walk keeps
    kMaxFind = 16,                      // pids per process name
};

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...)
{
    char text[1024];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    fprintf(stderr, "camera-ui: %s\n", text);
    fflush(stderr);
}

// ---- text, case-insensitively --------------------------------------------------------------------------------------

// Whole strings equal, whatever the case ("AXButton" and "axbutton" are the same role).
static int same_text(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && b[i]; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return a[i] == b[i];
}

// `needle` in `text`, whatever the case. An empty needle or an empty text matches nothing.
static int has_text(const char *text, const char *needle)
{
    size_t length;
    size_t i;
    size_t j;

    if (!text[0] || !needle[0])
        return 0;
    length = strlen(needle);
    for (i = 0; text[i]; i++) {
        for (j = 0; j < length && text[i + j]; j++) {
            if (tolower((unsigned char)text[i + j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == length)
            return 1;
    }
    return 0;
}

// ---- the processes -------------------------------------------------------------------------------------------------

// libproc's proc_name reports p_comm, which the kernel cuts at MAXCOMLEN (16) characters: "Photo Booth" arrives whole,
// "UserNotificationCenter" arrives as "UserNotification". A longer name is therefore also matched by that cut; the
// names used here do not collide with each other at 16 characters.
static int process_has_name(const char *process, const char *name)
{
    size_t length = strlen(name);

    if (strcmp(process, name) == 0)
        return 1;
    return length > 16 && strlen(process) == 16 && strncmp(process, name, 16) == 0;
}

// The pids whose process name is `name`; at most `capacity` of them.
static int find_pids(const char *name, pid_t *pids, int capacity)
{
    int room;
    int count;
    int found = 0;
    int i;
    pid_t *all;
    char process[kFieldSize];

    if (capacity <= 0)
        return 0;
    room = proc_listallpids(NULL, 0);
    if (room <= 0)
        return 0;
    room += 64;                         // processes come and go between the two calls
    all = calloc((size_t)room, sizeof(pid_t));
    if (!all)
        return 0;
    count = proc_listallpids(all, (int)((size_t)room * sizeof(pid_t)));
    if (count > room)
        count = room;
    for (i = 0; i < count && found < capacity; i++) {
        if (proc_name(all[i], process, (int)sizeof(process)) <= 0)
            continue;
        if (process_has_name(process, name))
            pids[found++] = all[i];
    }
    free(all);
    return found;
}

// ---- one app's accessibility tree ----------------------------------------------------------------------------------

struct ax_node {
    AXUIElementRef element;             // kept for the presses, released with the tree
    int parent;                         // -1 for a window (or for the app when it has no windows)
    int depth;
    char role[kFieldSize];
    char subrole[kFieldSize];
    char title[kFieldSize];
    char desc[kFieldSize];
    char value[kFieldSize];
    char identifier[kFieldSize];
    char help[kFieldSize];
};

struct ax_tree {
    int count;
    int full;                           // the node limit stopped the walk
    int windows;                        // window roots walked
    struct ax_node nodes[kMaxNodes];
};

// At most kFieldChars characters; ".." at the end says it was longer, and a UTF-8 character is never cut in half.
static void cut_field(char *text)
{
    size_t length = strlen(text);

    if (length <= kFieldChars)
        return;
    length = kFieldChars - 2;           // room for ".."
    while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80)
        length--;
    text[length] = '.';
    text[length + 1] = '.';
    text[length + 2] = '\0';
}

// A value as text: a string as it is, a Boolean as 1/0, a number as %g. Anything else (an array, a position, a size)
// says nothing to us here.
static void take_text(CFTypeRef value, char *out, size_t size)
{
    CFTypeID type;

    out[0] = '\0';
    if (!value)
        return;
    type = CFGetTypeID(value);
    if (type == CFStringGetTypeID()) {
        UInt8 bytes[256];
        CFIndex used = 0;

        CFStringGetBytes((CFStringRef)value, CFRangeMake(0, CFStringGetLength((CFStringRef)value)),
                         kCFStringEncodingUTF8, 0, 0, bytes, (CFIndex)sizeof(bytes) - 1, &used);
        if (used < 0 || used > (CFIndex)sizeof(bytes) - 1)
            used = 0;
        bytes[used] = '\0';
        snprintf(out, size, "%s", (const char *)bytes);
    } else if (type == CFBooleanGetTypeID()) {
        snprintf(out, size, "%d", CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0);
    } else if (type == CFNumberGetTypeID()) {
        double number = 0;

        if (CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &number))
            snprintf(out, size, "%g", number);
    }
    cut_field(out);
}

static CFTypeRef copy_attr(AXUIElementRef element, CFStringRef attribute)
{
    CFTypeRef value = NULL;

    if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess)
        return NULL;
    return value;
}

static void take_attr(AXUIElementRef element, CFStringRef attribute, char *out, size_t size)
{
    CFTypeRef value = copy_attr(element, attribute);

    take_text(value, out, size);
    if (value)
        CFRelease(value);
}

// The node for `element`, or -1 when the tree is full.
static int add_node(struct ax_tree *tree, AXUIElementRef element, int parent, int depth)
{
    struct ax_node *node;

    if (tree->count >= kMaxNodes) {
        tree->full = 1;
        return -1;
    }
    node = &tree->nodes[tree->count];
    node->element = (AXUIElementRef)CFRetain(element);
    node->parent = parent;
    node->depth = depth;
    take_attr(element, kAXRoleAttribute, node->role, sizeof(node->role));
    take_attr(element, kAXSubroleAttribute, node->subrole, sizeof(node->subrole));
    take_attr(element, kAXTitleAttribute, node->title, sizeof(node->title));
    take_attr(element, kAXDescriptionAttribute, node->desc, sizeof(node->desc));
    take_attr(element, kAXValueAttribute, node->value, sizeof(node->value));
    take_attr(element, kAXIdentifierAttribute, node->identifier, sizeof(node->identifier));
    take_attr(element, kAXHelpAttribute, node->help, sizeof(node->help));
    return tree->count++;
}

static void walk(struct ax_tree *tree, AXUIElementRef element, int parent, int depth)
{
    CFTypeRef children;
    int index = add_node(tree, element, parent, depth);
    CFIndex i;

    if (index < 0 || depth >= kMaxDepth)
        return;
    children = copy_attr(element, kAXChildrenAttribute);
    if (children && CFGetTypeID(children) == CFArrayGetTypeID()) {
        CFArrayRef list = (CFArrayRef)children;
        CFIndex count = CFArrayGetCount(list);

        for (i = 0; i < count && tree->count < kMaxNodes; i++) {
            CFTypeRef child = CFArrayGetValueAtIndex(list, i);

            if (child && CFGetTypeID(child) == AXUIElementGetTypeID())
                walk(tree, (AXUIElementRef)child, index, depth + 1);
        }
    }
    if (children)
        CFRelease(children);
}

static void tree_free(struct ax_tree *tree)
{
    int i;

    if (!tree)
        return;
    for (i = 0; i < tree->count; i++)
        CFRelease(tree->nodes[i].element);
    tree->count = 0;
    free(tree);
}

// The tree of one process: each of its windows is a root of its own (parent -1), so a window's subtree can be dumped on
// its own; the application element when there are no windows to walk (a panel-only process). The 1 s messaging timeout
// is set on the application element: every element of that process comes from its connection, so one call covers all.
static struct ax_tree *tree_of_app(pid_t pid)
{
    struct ax_tree *tree = calloc(1, sizeof(*tree));
    AXUIElementRef app;
    CFTypeRef windows;
    CFIndex count = 0;
    CFIndex i;

    if (!tree)
        return NULL;
    app = AXUIElementCreateApplication(pid);
    if (!app) {
        free(tree);
        return NULL;
    }
    (void)AXUIElementSetMessagingTimeout(app, 1.0f);
    windows = copy_attr(app, kAXWindowsAttribute);
    if (windows && CFGetTypeID(windows) == CFArrayGetTypeID())
        count = CFArrayGetCount((CFArrayRef)windows);
    for (i = 0; i < count; i++) {
        CFTypeRef window = CFArrayGetValueAtIndex((CFArrayRef)windows, i);

        if (window && CFGetTypeID(window) == AXUIElementGetTypeID()) {
            walk(tree, (AXUIElementRef)window, -1, 0);
            tree->windows++;
        }
    }
    if (tree->windows == 0)
        walk(tree, app, -1, 0);
    if (windows)
        CFRelease(windows);
    CFRelease(app);
    return tree;
}

// ---- looking through a tree ----------------------------------------------------------------------------------------

// A field of an "ax" line: "-" when the element has nothing there, so that an empty attribute cannot be mistaken for a
// missing one.
static const char *field(const char *text)
{
    return text[0] ? text : "-";
}

// Help alone counts: it is what a match may have found, and then the line is how the log shows it.
static int node_has_text(const struct ax_node *node)
{
    return node->title[0] || node->desc[0] || node->value[0] || node->identifier[0] || node->help[0];
}

// `needle` in any text field of the node.
static int node_has(const struct ax_node *node, const char *needle)
{
    return has_text(node->title, needle) || has_text(node->desc, needle) || has_text(node->value, needle) ||
           has_text(node->identifier, needle) || has_text(node->help, needle);
}

// The node's first text, for the log: its title, else its description, value, identifier or help.
static const char *node_label(const struct ax_node *node)
{
    if (node->title[0])
        return node->title;
    if (node->desc[0])
        return node->desc;
    if (node->value[0])
        return node->value;
    if (node->identifier[0])
        return node->identifier;
    if (node->help[0])
        return node->help;
    return node->role[0] ? node->role : "?";
}

static void print_node(const struct ax_node *node, const char *name)
{
    say("ax %s: d=%d %s/%s t=%s d=%s v=%s id=%s", name, node->depth, field(node->role), field(node->subrole),
        field(node->title), field(node->desc), field(node->value), field(node->identifier));
}

// Nodes are added depth-first, so a node's subtree is the run after it whose depth is deeper.
static int subtree_end(const struct ax_tree *tree, int index)
{
    int depth = tree->nodes[index].depth;
    int i = index + 1;

    while (i < tree->count && tree->nodes[i].depth > depth)
        i++;
    return i;
}

static int subtree_root(const struct ax_tree *tree, int index)
{
    while (tree->nodes[index].parent >= 0)
        index = tree->nodes[index].parent;
    return index;
}

static void dump_tree(const struct ax_tree *tree, const char *name, int max_lines)
{
    int shown = 0;
    int i;

    if (!tree)
        return;
    say("ax %s: %d nodes, %d window(s)%s", name, tree->count, tree->windows, tree->full ? ", the node limit cut it" : "");
    for (i = 0; i < tree->count && shown < max_lines; i++) {
        if (!node_has_text(&tree->nodes[i]))
            continue;
        print_node(&tree->nodes[i], name);
        shown++;
    }
    if (shown == 0) {
        say("ax %s: nothing with text", name);
        return;
    }
    for (; i < tree->count; i++) {
        if (node_has_text(&tree->nodes[i])) {
            say("ax %s: cut at %d lines", name, max_lines);
            break;
        }
    }
}

// One window (or whatever subtree) of a tree, at most max_lines lines of it.
static void dump_subtree(const struct ax_tree *tree, int root, const char *name, int max_lines)
{
    int end = subtree_end(tree, root);
    int shown = 0;
    int i;

    for (i = root; i < end && shown < max_lines; i++) {
        if (!node_has_text(&tree->nodes[i]))
            continue;
        print_node(&tree->nodes[i], name);
        shown++;
    }
    if (shown == 0)
        say("ax %s: the window has nothing with text", name);
}

// Every process of that name: its windows' lines, at most max_lines each. The number of processes dumped.
static int dump_app(const char *name, int max_lines)
{
    pid_t pids[kMaxFind];
    int count = find_pids(name, pids, kMaxFind);
    int i;

    if (count == 0) {
        say("%s is not running", name);
        return 0;
    }
    for (i = 0; i < count; i++) {
        struct ax_tree *tree = tree_of_app(pids[i]);

        if (!tree)
            continue;
        dump_tree(tree, name, max_lines);
        tree_free(tree);
    }
    return count;
}

// A row of a settings list: the roles System Settings' tables and outlines put around a control (an outline row is an
// AXRow with the AXOutlineRow subrole, so the subrole test covers it). The outline itself is not a row: taking it would
// make the switch search look at the whole page.
static int is_row(const struct ax_node *node)
{
    return same_text(node->role, "AXRow") || same_text(node->role, "AXCell") || same_text(node->role, "AXGroup") ||
           has_text(node->subrole, "row");
}

// The row a text belongs to: the nearest of up to 5 ancestors that is a row (or a cell/group), else the ancestor two
// levels up (a text inside a cell inside a row).
static int find_row(const struct ax_tree *tree, int index)
{
    int node = index;
    int up;

    for (up = 0; up < 5; up++) {
        node = tree->nodes[node].parent;
        if (node < 0)
            break;
        if (is_row(&tree->nodes[node]))
            return node;
    }
    node = tree->nodes[index].parent;
    if (node >= 0)
        node = tree->nodes[node].parent;
    return node >= 0 ? node : index;
}

// The switch inside a row: the check box System Settings draws for a setting, or a switch/toggle.
static int find_switch(const struct ax_tree *tree, int row)
{
    int end = subtree_end(tree, row);
    int i;

    for (i = row; i < end; i++) {
        const struct ax_node *node = &tree->nodes[i];

        if (same_text(node->role, "AXCheckBox") || same_text(node->subrole, "AXSwitch") ||
            same_text(node->subrole, "AXToggle"))
            return i;
    }
    return -1;
}

// What "on" reads as. Anything else (0, false, nothing at all) is treated as off: the switch is pressed, and the line
// says when its value read as nothing, so the log tells the two apart.
static int value_is_on(const char *value)
{
    return same_text(value, "1") || same_text(value, "true") || same_text(value, "on") || same_text(value, "yes");
}

// The row's (i) button: the one that talks about info or details, else the last button of the row.
static int find_info_button(const struct ax_tree *tree, int row)
{
    int end = subtree_end(tree, row);
    int last = -1;
    int i;

    for (i = row; i < end; i++) {
        const struct ax_node *node = &tree->nodes[i];

        if (!same_text(node->role, "AXButton"))
            continue;
        if (has_text(node->desc, "info") || has_text(node->title, "info") || has_text(node->help, "info") ||
            has_text(node->desc, "detail") || has_text(node->title, "detail") || has_text(node->help, "detail"))
            return i;
        last = i;
    }
    return last;
}

// The first button in the tree whose title or description is one of `titles`; -1 when there is none.
static int find_titled_button(const struct ax_tree *tree, const char *const *titles, int title_count)
{
    int i;
    int t;

    for (i = 0; i < tree->count; i++) {
        const struct ax_node *node = &tree->nodes[i];

        if (!same_text(node->role, "AXButton"))
            continue;
        for (t = 0; t < title_count; t++) {
            if (same_text(node->title, titles[t]) || same_text(node->desc, titles[t]))
                return i;
        }
    }
    return -1;
}

static int has_any(const struct ax_node *node, const char *const *words, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (has_text(node->title, words[i]) || has_text(node->desc, words[i]) || has_text(node->help, words[i]) ||
            has_text(node->identifier, words[i]))
            return 1;
    }
    return 0;
}

// How much a button looks like Photo Booth's shutter: "take" beats "shutter" and "capture", which beat "photo".
static int shutter_score(const struct ax_node *node)
{
    static const char *const take[] = {"take"};
    static const char *const shutter[] = {"shutter", "capture"};
    static const char *const photo[] = {"photo"};

    if (!same_text(node->role, "AXButton"))
        return 0;
    if (has_any(node, take, 1))
        return 3;
    if (has_any(node, shutter, 2))
        return 2;
    if (has_any(node, photo, 1))
        return 1;
    return 0;
}

static void press_element(AXUIElementRef element, const char *what)
{
    AXError error = AXUIElementPerformAction(element, kAXPressAction);

    say("press %s: AXError %d", what, (int)error);
}

// ---- enable-extension ----------------------------------------------------------------------------------------------

// One walk of System Settings: 1 the switch was pressed, 2 it was on already, 3 the row's (i) button was pressed (the
// sheet with the switch should be up next round), 0 no text matched, 4 the text is there but nothing to press yet.
// `label` says which text led there, for the log.
static int enable_round(struct ax_tree *tree, const char *needle, char *label, size_t label_size,
                        char pressed[][kFieldSize], int pressed_count)
{
    int matched = 0;
    int info = -1;
    int i;

    for (i = 0; i < tree->count; i++) {
        int row;
        int sw;

        if (!node_has(&tree->nodes[i], needle))
            continue;
        matched++;
        row = find_row(tree, i);
        if (row < 0)
            continue;
        sw = find_switch(tree, row);
        // SwiftUI wraps a label in groups of its own: when the nearest "row" holds neither a switch nor a button, climb
        // (at most 6 levels) to the first ancestor that holds one, so the label's own row is what is searched.
        if (sw < 0 && find_info_button(tree, row) < 0) {
            int up = tree->nodes[i].parent;
            int level;

            for (level = 0; up >= 0 && level < 6; level++, up = tree->nodes[up].parent) {
                if (find_switch(tree, up) >= 0 || find_info_button(tree, up) >= 0) {
                    row = up;
                    sw = find_switch(tree, up);
                    break;
                }
            }
        }
        if (sw >= 0) {
            snprintf(label, label_size, "%s", node_label(&tree->nodes[i]));
            if (value_is_on(tree->nodes[sw].value))
                return 2;
            press_element(tree->nodes[sw].element, "the switch");
            say("pressed the switch of %s%s", label, tree->nodes[sw].value[0] ? "" : " (its value read as nothing)");
            return 1;
        }
        if (info < 0) {
            int button = find_info_button(tree, row);

            if (button >= 0) {
                info = button;
                snprintf(label, label_size, "%s", node_label(&tree->nodes[i]));
            }
        }
    }
    if (matched == 0)
        return 0;
    if (info >= 0) {
        int already = 0;

        for (i = 0; i < pressed_count; i++) {
            if (same_text(pressed[i], label))
                already = 1;
        }
        if (!already) {
            press_element(tree->nodes[info].element, "the info button");
            say("pressed the info button of %s", label);
            return 3;
        }
    }
    return 4;
}

// The alert a switch can bring up is SecurityAgent's; its texts are what the log wants. 20 s of looking, then one line
// either way, and either way the command is done: nothing else can be pressed from here.
static void await_security_agent(void)
{
    int second;
    int seen = 0;

    for (second = 0; second < 20; second++) {
        pid_t pids[kMaxFind];
        int count = find_pids("SecurityAgent", pids, kMaxFind);
        int i;

        if (count > 0)
            seen = 1;
        for (i = 0; i < count; i++) {
            struct ax_tree *tree = tree_of_app(pids[i]);

            if (!tree)
                continue;
            if (tree->windows > 0) {
                say("SecurityAgent window up");
                dump_tree(tree, "SecurityAgent", 15);
                tree_free(tree);
                return;
            }
            if (tree->count > 1) {
                // No AXWindow listed, but the element tree behind the process has something: dump that.
                say("SecurityAgent up without an AXWindow");
                dump_tree(tree, "SecurityAgent", 15);
                tree_free(tree);
                return;
            }
            tree_free(tree);
        }
        sleep(1);
    }
    if (seen)
        say("SecurityAgent was there, no readable window in 20 s");
    else
        say("no SecurityAgent after the switch");
}

static int cmd_enable_extension(const char *needle, long seconds)
{
    char pressed[8][kFieldSize];        // the rows whose (i) button was pressed in earlier rounds
    int pressed_count = 0;
    int round = 0;
    int dumped = 0;
    int told_no_settings = 0;
    long elapsed = 0;
    char label[kFieldSize] = "";

    say("enable-extension: '%s' in System Settings, up to %ld s", needle, seconds);
    while (elapsed < seconds) {
        pid_t pids[kMaxFind];
        int count = find_pids("System Settings", pids, kMaxFind);
        int again = 0;
        int i;

        round++;
        if (count == 0 && !told_no_settings) {
            say("System Settings is not running");
            told_no_settings = 1;
        }
        for (i = 0; i < count && !again; i++) {
            struct ax_tree *tree = tree_of_app(pids[i]);
            int answer;

            if (!tree)
                continue;
            answer = enable_round(tree, needle, label, sizeof(label), pressed, pressed_count);
            if (answer == 1) {
                tree_free(tree);
                await_security_agent();
                return 0;
            }
            if (answer == 2) {
                say("the switch of %s is on already", label);
                tree_free(tree);
                return 0;
            }
            if (answer == 3) {
                if (pressed_count < (int)(sizeof(pressed) / sizeof(pressed[0])))
                    snprintf(pressed[pressed_count++], kFieldSize, "%s", label);
                again = 1;      // the next round walks again: the sheet with the switch should be there
            }
            if (answer == 0 && round >= 3 && !dumped) {
                dump_tree(tree, "System Settings", 120);
                dumped = 1;
            }
            tree_free(tree);
        }
        sleep(2);
        elapsed += 2;
    }
    say("%s not found / not switched in %ld s", needle, seconds);
    dump_app("System Settings", 120);
    return 1;
}

// ---- press-button --------------------------------------------------------------------------------------------------

// "any" means the windows a boot's alerts and settings come from, in the order they usually have something to press.
static const char *const kAnyApps[] = {"UserNotificationCenter", "CoreServicesUIAgent", "Photo Booth",
                                       "System Settings"};

static int cmd_press_button(const char *app, const char *titles_argument, long seconds)
{
    const char *names[8];
    char split[256];
    char shown[256];
    const char *titles[8];
    int title_count = 0;
    int name_count = 0;
    char *cursor;
    long elapsed;
    int c;

    if (strlen(titles_argument) >= sizeof(split)) {
        say("press-button: the titles are too long");
        return 2;
    }
    snprintf(split, sizeof(split), "%s", titles_argument);
    snprintf(shown, sizeof(shown), "%s", titles_argument);
    cursor = split;
    while (*cursor && title_count < (int)(sizeof(titles) / sizeof(titles[0]))) {
        char *comma = strchr(cursor, ',');

        if (comma)
            *comma = '\0';
        if (*cursor)
            titles[title_count++] = cursor;
        if (!comma)
            break;
        cursor = comma + 1;
    }
    if (title_count == 0) {
        say("press-button: no TITLE in '%s'", shown);
        return 2;
    }
    if (strcmp(app, "any") == 0) {
        for (c = 0; c < (int)(sizeof(kAnyApps) / sizeof(kAnyApps[0])); c++)
            names[name_count++] = kAnyApps[c];
    } else {
        names[name_count++] = app;
    }
    say("press-button: '%s' in %s, up to %ld s", shown, app, seconds);
    for (elapsed = 0; elapsed < seconds; elapsed++) {
        int ci;

        for (ci = 0; ci < name_count; ci++) {
            pid_t pids[kMaxFind];
            int count = find_pids(names[ci], pids, kMaxFind);
            int i;

            for (i = 0; i < count; i++) {
                struct ax_tree *tree = tree_of_app(pids[i]);
                int button;

                if (!tree)
                    continue;
                button = find_titled_button(tree, titles, title_count);
                if (button >= 0) {
                    const char *label = node_label(&tree->nodes[button]);
                    int root = subtree_root(tree, button);

                    press_element(tree->nodes[button].element, label);
                    say("pressed %s in %s", label, names[ci]);
                    dump_subtree(tree, root, names[ci], 15);
                    tree_free(tree);
                    return 0;
                }
                tree_free(tree);
            }
        }
        sleep(1);
    }
    say("no button %s in %ld s", shown, seconds);
    for (c = 0; c < name_count; c++) {
        pid_t pids[kMaxFind];
        int count = find_pids(names[c], pids, kMaxFind);
        int i;

        for (i = 0; i < count; i++) {
            struct ax_tree *tree = tree_of_app(pids[i]);

            if (!tree)
                continue;
            if (tree->windows > 0)
                dump_tree(tree, names[c], 40);
            tree_free(tree);
        }
    }
    return 1;
}

// ---- shoot ---------------------------------------------------------------------------------------------------------

static int cmd_shoot(long seconds)
{
    long elapsed;

    say("shoot: the shutter of Photo Booth, up to %ld s", seconds);
    for (elapsed = 0; elapsed < seconds; elapsed++) {
        pid_t pids[kMaxFind];
        int count = find_pids("Photo Booth", pids, kMaxFind);
        int i;

        for (i = 0; i < count; i++) {
            struct ax_tree *tree = tree_of_app(pids[i]);
            int best = -1;
            int best_score = 0;
            int node;

            if (!tree)
                continue;
            for (node = 0; node < tree->count; node++) {
                int score = shutter_score(&tree->nodes[node]);

                if (score > best_score) {
                    best_score = score;
                    best = node;
                }
            }
            if (best >= 0) {
                const char *label = node_label(&tree->nodes[best]);

                press_element(tree->nodes[best].element, label);
                say("pressed %s in Photo Booth", label);
                tree_free(tree);
                return 0;
            }
            tree_free(tree);
        }
        sleep(1);
    }
    say("no shutter in %ld s", seconds);
    dump_app("Photo Booth", 80);
    return 1;
}

// ---- main ----------------------------------------------------------------------------------------------------------

static long parse_seconds(const char *text, long fallback)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || value <= 0)
        return fallback;
    return value;
}

static void usage(void)
{
    say("usage: iceland-camera-ui enable-extension NEEDLE [SECONDS]");
    say("       iceland-camera-ui press-button APP TITLE[,TITLE...] [SECONDS]      APP or 'any'");
    say("       iceland-camera-ui shoot [SECONDS]");
    say("       iceland-camera-ui dump APP [MAX]");
}

int main(int argc, char **argv)
{
    say("trusted %d", AXIsProcessTrusted() ? 1 : 0);
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "enable-extension") == 0) {
        if (argc < 3) {
            say("enable-extension needs NEEDLE");
            return 2;
        }
        return cmd_enable_extension(argv[2], argc > 3 ? parse_seconds(argv[3], 60) : 60);
    }
    if (strcmp(argv[1], "press-button") == 0) {
        if (argc < 4) {
            say("press-button needs APP and TITLE");
            return 2;
        }
        return cmd_press_button(argv[2], argv[3], argc > 4 ? parse_seconds(argv[4], 30) : 30);
    }
    if (strcmp(argv[1], "shoot") == 0)
        return cmd_shoot(argc > 2 ? parse_seconds(argv[2], 30) : 30);
    if (strcmp(argv[1], "dump") == 0) {
        long lines;

        if (argc < 3) {
            say("dump needs APP");
            return 2;
        }
        lines = argc > 3 ? parse_seconds(argv[3], 120) : 120;
        return dump_app(argv[2], lines > 1000 ? 1000 : (int)lines) > 0 ? 0 : 1;
    }
    say("unknown command '%s'", argv[1]);
    usage();
    return 2;
}
