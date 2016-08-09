// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "host_backend.h"

#include <core/path.h>
#include <core/settings.h>
#include <core/str.h>
#include <lib/line_buffer.h>
#include <terminal/terminal.h>

#if MODE4
    "M-h:   show-rl-help",
    "C-z:   undo"
#endif // MODE4

//------------------------------------------------------------------------------
static setting_enum g_paste_crlf(
    "clink.paste_crlf",
    "Strips CR and LF chars on paste",
    "Setting this to a value >0 will make Clink strip CR and LF characters\n"
    "from text pasted into the current line. Set this to 1 to strip all\n"
    "newline characters and 2 to replace them with a space.",
    "unchanged,delete,space",
    1);



//------------------------------------------------------------------------------
static void ctrl_c(
    editor_backend::result& result,
    const editor_backend::context& context)
{
    //auto& buffer = context.buffer;
    context.buffer.remove(0, ~0u);
    context.terminal.write("\n^C\n", 4);
    result.redraw();

#if 0
    DWORD mode;
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
    {
        if (mode & ENABLE_PROCESSED_INPUT)
        {
            // Fire a Ctrl-C event and stop Readline. ReadConsole would also
            // set error 0x3e3 (ERROR_OPERATION_ABORTED) too.
            GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
            Sleep(5);

            SetLastError(0x3e3);
        }
    }
#endif // 0
}

//------------------------------------------------------------------------------
static void strip_crlf(char* line)
{
    int setting = g_paste_crlf.get();
    if (setting <= 0)
        return;

    int prev_was_crlf = 0;
    char* write = line;
    const char* read = line;
    while (*read)
    {
        char c = *read;
        if (c != '\n' && c != '\r')
        {
            prev_was_crlf = 0;
            *write = c;
            ++write;
        }
        else if (setting > 1 && !prev_was_crlf)
        {
            prev_was_crlf = 1;
            *write = ' ';
            ++write;
        }

        ++read;
    }

    *write = '\0';
}

//------------------------------------------------------------------------------
static void paste(line_buffer& buffer)
{
    if (OpenClipboard(nullptr) == FALSE)
        return;

    HANDLE clip_data = GetClipboardData(CF_UNICODETEXT);
    if (clip_data != nullptr)
    {
        str<1024> utf8;
        to_utf8(utf8, (wchar_t*)clip_data);

        strip_crlf(utf8.data());
        buffer.insert(utf8.c_str());
    }

    CloseClipboard();
}

//------------------------------------------------------------------------------
static void copy_impl(const char* value, int length)
{
    int size = (length + 4) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (mem == nullptr)
        return;

    wchar_t* data = (wchar_t*)GlobalLock(mem);
    length = to_utf16((wchar_t*)data, length + 1, value);
    GlobalUnlock(mem);

    if (OpenClipboard(nullptr) == FALSE)
        return;

    SetClipboardData(CF_TEXT, nullptr);
    SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
}

//------------------------------------------------------------------------------
static void copy_line(const line_buffer& buffer)
{
    copy_impl(buffer.get_buffer(), buffer.get_length());
}

//------------------------------------------------------------------------------
static void copy_cwd(const line_buffer& buffer)
{
    str<270> cwd;
    unsigned int length = GetCurrentDirectory(cwd.size(), cwd.data());
    if (length < cwd.size())
    {
        cwd << "\\";
        path::clean(cwd);
        copy_impl(cwd.c_str(), cwd.length());
    }
}

//------------------------------------------------------------------------------
static void up_directory(editor_backend::result& result, line_buffer& buffer)
{
    buffer.begin_undo_group();
    buffer.remove(0, ~0u);
    buffer.insert(" cd ..");
    buffer.end_undo_group();
    result.done();
}

//------------------------------------------------------------------------------
static void get_word_bounds(const line_buffer& buffer, int* left, int* right)
{
    const char* str = buffer.get_buffer();
    unsigned int cursor = buffer.get_cursor();

    // Determine the word delimiter depending on whether the word's quoted.
    int delim = 0;
    for (unsigned int i = 0; i < cursor; ++i)
    {
        char c = str[i];
        delim += (c == '\"');
    }

    // Search outwards from the cursor for the delimiter.
    delim = (delim & 1) ? '\"' : ' ';
    *left = 0;
    for (int i = cursor - 1; i >= 0; --i)
    {
        char c = str[i];
        if (c == delim)
        {
            *left = i + 1;
            break;
        }
    }

    const char* post = strchr(str + cursor, delim);
    if (post != nullptr)
        *right = int(post - str);
    else
        *right = int(strlen(str));
}

//------------------------------------------------------------------------------
static void expand_env_vars(line_buffer& buffer)
{
    // Extract the word under the cursor.
    int word_left, word_right;
    get_word_bounds(buffer, &word_left, &word_right);

    str<1024> in;
    in << (buffer.get_buffer() + word_left);
    in.truncate(word_right - word_left);

    // Do the environment variable expansion.
    str<1024> out;
    if (!ExpandEnvironmentStrings(in.c_str(), out.data(), out.size()))
        return;

    // Update Readline with the resulting expansion.
    buffer.begin_undo_group();
    buffer.remove(word_left, word_right);
    buffer.set_cursor(word_left);
    buffer.insert(out.c_str());
    buffer.end_undo_group();
}

//------------------------------------------------------------------------------
static void insert_dot_dot(line_buffer& buffer)
{
    if (unsigned int cursor = buffer.get_cursor())
    {
        char last_char = buffer.get_buffer()[cursor - 1];
        if (last_char != ' ' && !path::is_separator(last_char))
            buffer.insert("\\");
    }

    buffer.insert("..\\");
}



//------------------------------------------------------------------------------
enum
{
    bind_id_paste,
    bind_id_ctrlc,
    bind_id_copy_line,
    bind_id_copy_cwd,
    bind_id_up_dir,
    bind_id_expand_env,
    bind_id_dotdot,
};

//------------------------------------------------------------------------------
void host_backend::bind_input(binder& binder)
{
    int default_group = binder.get_group();
    binder.bind(default_group, "^v", bind_id_paste);
    binder.bind(default_group, "^c", bind_id_ctrlc);
    binder.bind(default_group, "\\M-C-c", bind_id_copy_line);
    binder.bind(default_group, "\\M-C", bind_id_copy_cwd);
    binder.bind(default_group, "\\eO5", bind_id_up_dir);
    binder.bind(default_group, "\\M-C-e", bind_id_expand_env);
    binder.bind(default_group, "\\M-a", bind_id_dotdot);
}

//------------------------------------------------------------------------------
void host_backend::on_begin_line(const char* prompt, const context& context)
{
}

//------------------------------------------------------------------------------
void host_backend::on_end_line()
{
}

//------------------------------------------------------------------------------
void host_backend::on_matches_changed(const context& context)
{
}

//------------------------------------------------------------------------------
void host_backend::on_input(const input& input, result& result, const context& context)
{
    switch (input.id)
    {
    case bind_id_paste:         paste(context.buffer);                break;
    case bind_id_ctrlc:         ctrl_c(result, context);              break;
    case bind_id_copy_line:     copy_line(context.buffer);            break;
    case bind_id_copy_cwd:      copy_cwd(context.buffer);             break;
    case bind_id_up_dir:        up_directory(result, context.buffer); break;
    case bind_id_expand_env:    expand_env_vars(context.buffer);      break;
    case bind_id_dotdot:        insert_dot_dot(context.buffer);       break;
    };
}