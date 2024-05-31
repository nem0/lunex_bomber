project "game"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_GAME" }
	links { "engine" }
	useLua()
	defaultConfigurations()

linkPlugin("game")