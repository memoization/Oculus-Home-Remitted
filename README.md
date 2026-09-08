## Oculus Home Remitted [![Github All Releases](https://img.shields.io/github/downloads/memoization/Oculus-Home-Remitted/total)](https://github.com/memoization/Oculus-Home-Remitted/releases/latest)
<img width="1280" height="720" alt="remitted_cover_thumb_720" src="https://github.com/user-attachments/assets/fa2b7058-b1a9-48cc-bfdf-cf5268e703b3" />


# About
Oculus Home Remitted is a project to revive Oculus Home (or known as Rift Core 2.0) experiences for full use offline. This aims to work with modern *Meta Link* software for newer headsets which eliminates the need of tethering to an old version of the Oculus client.

Meta shutdown access to Oculus Home years ago evicting many users of their digital spaces which often many consist of custom UGC assets. The base app is heavily dependent on a remote backend to start and to perform vital functions. The tool mimics those vital parts of the backend allowing game functionality fully local and offline.

### Notable features:
- Set a custom profile name and icon. Both can appear in VR.
- Locally stored list of homes which allows setting what default home you land in at startup. Editing a home's object layout or changing its information persists offline. VR actions such as naming a home, creating a home, or deleting a home are supported.
- Includes a feature for fetching your old homes from your Oculus (Meta) account. This will only work while Meta has these endpoints and data still available. ***Best to act on that now if you want to preserve your old experiences!***
- Supports loading your Oculus app library offline.
- Supports fetching all of your Oculus app achievements.
- Allows usage of embedded-panels (or broadcasting to screens). This enables viewing your desktop with officially interactive screen objects in VR.

# Setup Guide
This tool only supports a specific version of Oculus Home. It can be downloaded from [archive.org](https://archive.org/download/oculus-worlds/worlds-2021-03-29.zip)

1. Extract the *oculus-worlds* folder at any location.
2. Download the latest version of the [tool](https://github.com/memoization/Oculus-Home-Remitted/releases/latest). The tool is portable as there is no installer.
3. Extract and launch *Home2Remitted.exe*.
4. Use the *Set Executable* option then navigate to the *oculus-worlds* folder you extracted and select `Home2-Win64-Shipping.exe` located at `Home2\Binaries\Win64`. `Home2.exe` at the root should be ignored.
5. Launch the Meta Link software and have its dash running. You will need to enable "Unknown sources" in the settings page.
6. Use the *Launch Home* option in the tool. The Oculus Home process will now start and the tool will spin up its backend to take over.
7. Now you should find yourself inside Oculus Home in a default home layout fully offline.

## Revive setup for using a SteamVR headset
1. Grab a copy of [Revive](https://github.com/LibreVR/Revive/releases) and install it. Using Revive with Oculus Home is confirmed working as of its version `3.2.0`.
2. Go to the Oculus program files *Support* directory in explorer: `C:\Program Files\Oculus\Support`
3. Take the *oculus-worlds* folder you downloaded earlier and place it into the Oculus directory in explorer.
4. Launch Revive Dashboard. It should show an Oculus Home icon similar to the snapshot. <img width="1019" height="281" alt="image" src="https://github.com/user-attachments/assets/65d8575b-f914-4127-bf21-5e8463f1985d" />
5. As long as the *Oculus Home Remitted* tool is running, launching Oculus Home through the *Revive Dashboard* should get you running in your SteamVR headset. Having Meta Link software running is not required!

*While using Revive, controller inputs may be a bit off by default compared to running a native Oculus headset. Adjust your bindings using SteamVR's *Manage Controller Bindings* for the *Home2-Win64-Shipping* app while it is running.*

*At this time, embedded-panels (or broadcasting to screen objects) do not work with Revive*

# Caveats
The following features of Oculus Home are not supported by the tool at this time. Offline support for some may drop later..
- Editing and customizing avatars.
- Launching Oculus app titles from the game cartridge feature.
- Uploading new UGC content (at least a UX-friendly way from in VR). You can possibly add new UGC by manually configuring your home's `config.json` and adding entries to its `ugc-hashes.json`.
- Any multiplayer features are out of scope of this project and unlikely to ever be supported in this repository. Reversing the *VertsClient.dll* via thorough capture of verts traffic in a live multiplayer session would be needed as a rough start. There are capture tools provided in the repository for those who are adventurous.
