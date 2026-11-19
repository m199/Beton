/*
 * Copyright 2005-2026, Daniel Weber.
 * Copyright 2026, David Beckman, indispensablelabs@gmail.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "Debug.h"
#include "MainWindow.h"
#include "Messages.h"
#include <Application.h>
#include <Catalog.h>
#include <cstring>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Application"

bool gIsDebug = false;

static void _MakeShortcutSpec(BMessage* spec, const char* appPath, const char* arg, int32 key) {
  spec->MakeEmpty();
  spec->AddString("class", "ShortcutsSpec");
  spec->AddString("class", "ShortcutsSpec");

  char command[512];
  snprintf(command, sizeof(command), "%s %s", appPath, arg);
  spec->AddString("command", command);
  spec->AddInt32("key", key);
  spec->AddInt32("mcidx", 0);
  spec->AddInt32("mcidx", 0);
  spec->AddInt32("mcidx", 0);
  spec->AddInt32("mcidx", 0);

  BMessage modtester;
  modtester.AddString("class", "MinMatchFieldTester");
  int mods[] = {1, 4, 2, 64};
  for (int i = 0; i < 4; ++i) {
    BMessage slave;
    slave.AddString("class", "HasBitsFieldTester");
    slave.AddInt32("rqBits", 0);
    slave.AddInt32("fbBits", mods[i]);
    modtester.AddMessage("mSlave", &slave);
  }
  modtester.AddInt32("mMin", 4);
  spec->AddMessage("modtester", &modtester);

  BMessage act;
  act.AddString("class", "LaunchCommandActuator");
  act.AddString("largv", appPath);
  act.AddString("largv", arg);
  spec->AddMessage("act", &act);
}

/**
 * @class BetonApp
 * @brief The Application class.
 *
 * Initializes the application and creates the main window.
 */
class BetonApp : public BApplication {
public:
  BetonApp() : BApplication("application/x-vnd.Beton"), fPendingCommand(0) {}

  void ReadyToRun() override {
    MainWindow *window = new MainWindow();
    window->Show();

    app_info info;
    if (GetAppInfo(&info) == B_OK) {
      BPath path(&info.ref);
      RegisterShortcuts(path.Path());
    }

    if (fPendingCommand != 0) {
      window->PostMessage(fPendingCommand);
    }
  }

  void SendCommandToWindow(uint32 command) {
    for (int32 i = 0; ; ++i) {
      BWindow *win = WindowAt(i);
      if (!win)
        break;
      if (dynamic_cast<MainWindow *>(win)) {
        win->PostMessage(command);
        break;
      }
    }
  }

  void ArgvReceived(int32 argc, char **argv) override {
    uint32 cmd = 0;
    for (int32 i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--play-pause") == 0 || strcmp(argv[i], "play-pause") == 0 || strcmp(argv[i], "ppau") == 0) {
        cmd = MSG_PLAYPAUSE;
      } else if (strcmp(argv[i], "--stop") == 0 || strcmp(argv[i], "stop") == 0) {
        cmd = MSG_STOP;
      } else if (strcmp(argv[i], "--next") == 0 || strcmp(argv[i], "next") == 0) {
        cmd = MSG_PLAY_NEXT;
      } else if (strcmp(argv[i], "--prev") == 0 || strcmp(argv[i], "prev") == 0 || strcmp(argv[i], "prvs") == 0) {
        cmd = MSG_PREV_SONG;
      }
    }

    if (cmd != 0) {
      bool launching = true;
      for (int32 i = 0; ; ++i) {
        BWindow *win = WindowAt(i);
        if (!win)
          break;
        if (dynamic_cast<MainWindow *>(win)) {
          launching = false;
          win->PostMessage(cmd);
          break;
        }
      }
      if (launching) {
        fPendingCommand = cmd;
      }
    }
  }

  void RegisterShortcuts(const char* appPath) {
    BPath settingsPath;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath) != B_OK)
      return;
    settingsPath.Append("shortcuts_settings");

    BFile file(settingsPath.Path(), B_READ_WRITE | B_CREATE_FILE);
    if (file.InitCheck() != B_OK)
      return;

    BMessage msg;
    off_t size = 0;
    file.GetSize(&size);
    bool exists = (size > 0);
    if (exists) {
      if (msg.Unflatten(&file) != B_OK) {
        return;
      }
    }

    struct {
      int32 key;
      const char* arg;
    } targets[] = {
      {0xc00cd, "--play-pause"},
      {0xc00b7, "--stop"},
      {0xc00b5, "--next"},
      {0xc00b6, "--prev"}
    };

    bool changed = false;

    for (int t = 0; t < 4; ++t) {
      int32 key = targets[t].key;
      const char* arg = targets[t].arg;

      bool found = false;
      BMessage spec;
      int32 i = 0;
      while (msg.FindMessage("spec", i, &spec) == B_OK) {
        int32 specKey = 0;
        spec.FindInt32("key", &specKey);
        if (specKey == key) {
          const char* currentCommand = spec.FindString("command");
          char expectedCommand[512];
          snprintf(expectedCommand, sizeof(expectedCommand), "%s %s", appPath, arg);
          if (!currentCommand || strcmp(currentCommand, expectedCommand) != 0) {
            BMessage newSpec;
            _MakeShortcutSpec(&newSpec, appPath, arg, key);
            msg.ReplaceMessage("spec", i, &newSpec);
            changed = true;
          }
          found = true;
          break;
        }
        i++;
      }

      if (!found) {
        BMessage newSpec;
        _MakeShortcutSpec(&newSpec, appPath, arg, key);
        msg.AddMessage("spec", &newSpec);
        changed = true;
      }
    }

    if (changed) {
      file.Seek(0, SEEK_SET);
      file.SetSize(0);
      msg.Flatten(&file);
    }
  }


  void MessageReceived(BMessage *msg) override {
    switch (msg->what) {
      case MSG_PLAYPAUSE:
      case MSG_PLAY:
      case MSG_PAUSE:
      case MSG_STOP:
      case MSG_PLAY_NEXT:
      case MSG_PREV_SONG: {
        for (int32 i = 0; ; ++i) {
          BWindow *win = WindowAt(i);
          if (!win)
            break;
          if (dynamic_cast<MainWindow *>(win)) {
            win->PostMessage(msg);
            break;
          }
        }
        break;
      }
      default:
        BApplication::MessageReceived(msg);
        break;
    }
  }
private:
  uint32 fPendingCommand;
};

/**
 * @brief Moves settings from the old "BeTon" directory to "Beton".
 *
 * The application was renamed from BeTon to Beton, and every settings path
 * below now says "Beton". Anyone upgrading from an earlier release has their
 * library cache, playlists, radio stations and music sources sitting in
 * ~/config/settings/BeTon, so without this they would start to an empty
 * application with no indication of why.
 *
 * Contents are moved rather than the directory being renamed, because the new
 * directory may already exist: the direct-output crash breadcrumb has always
 * been written to "Beton". Anything already present in the new location wins
 * and the old copy is left alone, so this cannot overwrite newer settings and
 * is safe to run on every start.
 *
 * BFS is case-sensitive, so "BeTon" and "Beton" really are distinct
 * directories and this is a genuine move, not a no-op.
 */
/**
 * @brief Repoints absolute paths that still name the old settings directory.
 *
 * Moving the files is not enough on its own: the settings file stores
 * `playlist_path` as an absolute path, so after a move the application would
 * recreate an empty Playlists folder under the old name and report that the
 * user has no playlists, while their .m3u files sat untouched in the new
 * directory.
 *
 * Every top-level string is checked rather than just the one known key, so a
 * setting added later gets the same treatment. The prefix test means paths
 * that point somewhere else entirely -- music folders, in particular -- are
 * left alone.
 *
 * @return true if anything was rewritten.
 */
static bool RewriteLegacySettingsPaths(BMessage &settings,
                                       const BString &legacyPrefix,
                                       const BString &currentPrefix) {
  bool changed = false;
  char *name = NULL;
  type_code type = B_ANY_TYPE;
  int32 count = 0;

  for (int32 i = 0;
       settings.GetInfo(B_ANY_TYPE, i, &name, &type, &count) == B_OK; i++) {
    if (type != B_STRING_TYPE)
      continue;

    for (int32 j = 0; j < count; j++) {
      BString value;
      if (settings.FindString(name, j, &value) != B_OK)
        continue;
      if (!value.StartsWith(legacyPrefix))
        continue;

      BString updated(value);
      updated.ReplaceFirst(legacyPrefix.String(), currentPrefix.String());
      if (settings.ReplaceString(name, j, updated.String()) == B_OK) {
        DEBUG_PRINT("Repointed setting '%s': %s -> %s\n", name, value.String(),
                    updated.String());
        changed = true;
      }
    }
  }

  return changed;
}

static void MigrateLegacySettingsDir() {
  BPath base;
  if (find_directory(B_USER_SETTINGS_DIRECTORY, &base) != B_OK)
    return;

  BPath legacyPath(base);
  legacyPath.Append("BeTon");
  BDirectory legacyDir(legacyPath.Path());
  if (legacyDir.InitCheck() != B_OK)
    return; // No old settings: nothing to do.

  BPath currentPath(base);
  currentPath.Append("Beton");
  create_directory(currentPath.Path(), 0755);
  BDirectory currentDir(currentPath.Path());
  if (currentDir.InitCheck() != B_OK)
    return;

  int32 moved = 0;
  BEntry entry;
  legacyDir.Rewind();
  while (legacyDir.GetNextEntry(&entry) == B_OK) {
    char name[B_FILE_NAME_LENGTH];
    if (entry.GetName(name) != B_OK)
      continue;
    if (currentDir.Contains(name))
      continue; // Already migrated, or newer: keep what is there.
    if (entry.MoveTo(&currentDir) == B_OK)
      moved++;
  }

  // Settings that name the old directory by absolute path have to follow the
  // files, or the application recreates the old layout and reports the data
  // as missing.
  BPath settingsFile(currentPath);
  settingsFile.Append("settings");
  BFile file(settingsFile.Path(), B_READ_ONLY);
  BMessage settings;
  if (file.InitCheck() == B_OK && settings.Unflatten(&file) == B_OK) {
    file.Unset();

    if (RewriteLegacySettingsPaths(settings, legacyPath.Path(),
                                   currentPath.Path())) {
      // Written via a temp file and renamed, so an interrupted write cannot
      // leave the user with a half-written settings file.
      BString tempPath(settingsFile.Path());
      tempPath << ".new";

      BFile out(tempPath.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
      if (out.InitCheck() == B_OK && settings.Flatten(&out) == B_OK) {
        out.Sync();
        out.Unset();
        if (rename(tempPath.String(), settingsFile.Path()) != 0)
          unlink(tempPath.String());
      } else {
        out.Unset();
        unlink(tempPath.String());
      }
    }
  }

  // Only remove the old directory once it is genuinely empty, so anything
  // that could not be moved stays where the user can still find it.
  legacyDir.Rewind();
  if (legacyDir.CountEntries() == 0) {
    BEntry legacyEntry(legacyPath.Path());
    legacyEntry.Remove();
  }

  if (moved > 0) {
    DEBUG_PRINT("Migrated %ld settings item(s) from BeTon/ to Beton/\n",
                (long)moved);
  }
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--debug") == 0) {
      gIsDebug = true;
    }
  }

  if (!gIsDebug) {

    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
  }

  if (gIsDebug) {
    DEBUG_PRINT("Starting in DEBUG mode\n");
  }

  // Before anything reads settings: the window, the cache looper and the
  // radio library all resolve their paths during construction.
  MigrateLegacySettingsDir();

  BetonApp app;
  app.Run();
  return 0;
}
