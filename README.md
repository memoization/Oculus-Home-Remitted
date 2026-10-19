## Oculus Home Remitted [![Github All Releases](https://img.shields.io/github/downloads/memoization/Oculus-Home-Remitted/total)](https://github.com/memoization/Oculus-Home-Remitted/releases/latest)
<img width="1280" height="720" alt="remitted_cover_thumb_720" src="https://github.com/user-attachments/assets/fa2b7058-b1a9-48cc-bfdf-cf5268e703b3" />


# About
Oculus Home Remitted is a project to revive Oculus Home (or known as Rift Core 2.0) experiences for full use offline. This aims to work with modern *Meta Link* software for newer headsets which eliminates the need of tethering to an old version of the Oculus client.

Meta shutdown access to Oculus Home years ago evicting many users of their digital spaces which often many consist of custom UGC assets. The base app is heavily dependent on a remote backend to start and to perform vital functions. The tool mimics those vital parts of the backend allowing game functionality fully local and offline.

### Notable features:
- Set a custom profile name and icon. Both can appear in VR.
- Locally stored list of homes which allows setting what default home you land in at startup. Editing a home's object layout or changing its information persists offline. VR actions such as naming a home, creating a home, or deleting a home are supported.
- Includes a feature for fetching your old homes from your Oculus (Meta) account. This will only work while Meta has these endpoints and data still available. ***Best to act on that now if you want to preserve your old experiences!***
- Supports loading in your Oculus and Steam app libraries.
- Supports fetching all of your Oculus app achievements and your achievements for installed Steam apps.
- Allows usage of embedded-panels (or broadcasting to screens). This enables viewing your desktop with officially interactive screen objects in VR.
- Auto-launch Oculus Home from either Oculus or SteamVR dashboard, similar to the old Oculus Dash behavior.
- Supports all UGC custom import features. Importing a UGC asset will register it locally to use offline.

# Help Wanted: Finding Lost Contest Assets
Many years ago, Oculus Medium held two contests for users to submit their handmade sculptures: "_Pumpkin Sculpting Competition_" and "_Snowman Sculpting Competition_".
The top winners were given a 3D print of their submission and their sculpture to be featured in Oculus Home for a limited time. Unfortunately, the time window was very short to fetch these assets.

The *Pumpkin Sculpting Competition* winners have been archived already:
- `Halloween Pumpkin`
- `Pumpkin Ghoul`

👉 The *Snowman Sculpting Competition* sculpture winner models are not archived and likely to become lost media:
- `Snowman Attack`
- `Snowmen Decorating`

If you opened or used Oculus Home during the time window of these being featured, you may have seen them in your imports list and thus may be on-file in present day. 
1. Check WorldsCache by pasting this path in Explorer `%UserProfile%/AppData/Local/Home2/WorldsCache` and see if any `.zst` files are around the date of `1/18/2019 - 1/31/2019`. If you have any from around those dates, compress the file(s) to a `.zip` then submit [an issue](https://github.com/memoization/Oculus-Home-Remitted/issues/new?template=BLANK_ISSUE) with the file uploaded.
2. If one of your fetched homes you downloaded from your account has either of these assets, export your home from the *Homes* page and compress the `.ochome` file to `.zip` then submit [an issue](https://github.com/memoization/Oculus-Home-Remitted/issues/new?template=BLANK_ISSUE) with the file uploaded.
3. If your *My Imports* list has these listed while in Oculus Home, check the `Documents\Oculus Home\_Import` location for them. If you see them, compress the file(s) to a `.zip` then submit [an issue](https://github.com/memoization/Oculus-Home-Remitted/issues/new?template=BLANK_ISSUE) with the file uploaded.

<img src="https://github.com/user-attachments/assets/5e3e9911-a124-428b-9075-fe1acb980dd5" width="600" alt="Asset Examples">

_Any public efforts to help with this would be greatly appreciated!_

# Setup Guide
This tool only supports a specific version of Oculus Home. **It can be downloaded [here](https://archive.org/download/oculus-worlds/worlds-2021-03-29.zip).**

1. Extract the *oculus-worlds* folder at any location.
2. Download the latest version of the [tool](https://github.com/memoization/Oculus-Home-Remitted/releases/latest). This is portable as there is no installer.
3. Extract and launch *Home2Remitted.exe*.
4. Use the *Set Executable* option then navigate to the *oculus-worlds* folder you extracted and select `Home2-Win64-Shipping.exe` located at `Home2\Binaries\Win64`. `Home2.exe` at the root should be ignored.
5. Launch the Meta Link software and have its dash running. You will need to enable "Unknown sources" in the settings page.
6. Use the *Launch Home* option in the tool. The Oculus Home process will now start and the tool will spin up its backend to take over.
7. Now you should find yourself inside Oculus Home in a default home layout fully offline.

## Revive setup for using a SteamVR headset
1. Grab a copy of [Revive](https://github.com/LibreVR/Revive/releases) and install it. Using Revive with Oculus Home is confirmed working as of its version `3.2.0`.
2. Go to the Oculus program files *Support* directory in explorer: `C:\Program Files\Oculus\Support`
3. Take the *oculus-worlds* folder you downloaded earlier and place it into that Oculus directory in explorer.
4. In the *Oculus Home Remitted* tool, go to *Settings* and enable *Launch with Revive*. Then set the location of *ReviveInjector.exe*.
5. You can now run Oculus Home with your SteamVR headset. Having Meta Link software running is not required but **it does need to be installed**!

*While using Revive, controller inputs may be a bit off by default compared to running a native Oculus headset. Adjust your bindings using SteamVR's *Manage Controller Bindings* for the running app.*

*At this time, embedded-panels (or broadcasting to screen objects) do not work with Revive*

# Caveats
The following features of Oculus Home are not supported by the tool at this time.
- Multiplayer features are out of scope of this project and _unlikely_ to ever be supported in this repository. Reversing the *VertsClient.dll* via thorough capture of verts traffic in a live multiplayer session would be needed as a rough start. There are capture tools available in the settings page for those who are adventurous.
