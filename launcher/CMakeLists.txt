# The launcher players start (later shipped as an AppImage): it builds the
# game program from the player's own .wua on their PC (builder/: ttt2-build
# and the kit it uses), installs and uninstalls the game, starts it, and adds
# Tekken 7's characters through tools/characters (app/: GTK 4; core/: its
# logic without GTK).
add_subdirectory(builder)
add_subdirectory(core)
add_subdirectory(app)
