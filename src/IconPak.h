#pragma once
#include <string>

namespace iconpak {

	// HACK HACK HACK
	// Builds a UE4.20 patch pak overriding the game's ui_tray_icon_profile texture into <home2ExeDir>\..\..\Content\Paks, from the given PNG image.
	// The texture is generated fresh: The image is center-cropped, resized to 512x512, mip-chained and then DXT1-encoded (1-bit alpha) into a cooked Texture2D.
	// Returns true on success, and on failure sets errOut. If home2ExePath is empty the caller hasn't set the executable, so it returns false with an error.
	bool BuildProfilePak(const std::string& home2ExePath, const std::string& imagePath, std::string& errOut);

}
