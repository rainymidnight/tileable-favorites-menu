# Tileable Favorites Menu
Tileable Favorites Menu is a complete replacement of the vanilla/SkyUI favorites list. TFM presents favorites as a space-maximizing grid of movable tiles, designed for fast access and low maintenance. No special keybinds are required: simply favorite items through the vanilla system, and they will appear in the menu. Individual tiles can be dragged to arrange the layout to your liking. TFM can display many items on a single screen.

## Build

Requires Visual Studio 2022, CMake, Ninja, and vcpkg (`VCPKG_ROOT` set).

```powershell
cmake --preset release
cmake --build --preset release
```

Output: `build/release/TileableFavoritesMenu.dll`
