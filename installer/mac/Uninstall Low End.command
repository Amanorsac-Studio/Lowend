#!/bin/bash
# Uninstall Low End.command - removes everything the Low End installer placed.
# Your own presets, songs and stems in ~/Documents/Amanorsac Studio/Low End are
# NOT touched. Delete that folder yourself if you want them gone too.
echo "This removes Low End from this Mac. Your presets, songs and stems are kept."
read -r -p "Remove Low End? (y/n) " answer
[ "$answer" = "y" ] || { echo "Nothing was removed."; exit 0; }
sudo rm -rf "/Applications/Low End.app" \
            "/Library/Audio/Plug-Ins/VST3/Low End.vst3" \
            "/Library/Audio/Plug-Ins/Components/Low End.component" \
            "/Library/Application Support/Amanorsac Studio/Low End"
for id in app vst3 au model docs; do sudo pkgutil --forget "com.amanorsac.lowend.$id" >/dev/null 2>&1; done
rm -rf "$HOME/Library/Application Support/Amanorsac Studio/Low End"
echo "Low End has been removed. Your own files are still in ~/Documents/Amanorsac Studio/Low End."
