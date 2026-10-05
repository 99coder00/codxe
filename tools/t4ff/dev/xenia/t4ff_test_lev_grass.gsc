// t4ff test: CoD Xenon's Leviathan (nazi_zombie_leviathan). Its grass clumps
// (foliage_grass_short_squareclump, static models) draw technique set mc_ambient_t0c0, whose vertex
// shaders CoD Xenon compiled with vertex fetches: the game binds them without a vertex declaration
// (as Kino Rezurrection's dry grass did, which froze the game). Stands by them, four views.
#include maps\_utility;

init()
{
	level thread run();
}

step(text)
{
	SetDvar("t4ff_test", text);
}

run()
{
	step("waiting for the player");
	players = get_players();
	while (players.size == 0 || !IsAlive(players[0]))
	{
		wait 0.5;
		players = get_players();
	}
	player = players[0];
	player EnableInvulnerability();
	wait 5;
	step("start at " + player.origin);
	// the clumps are around (5600-6250, -2800 - -2190, -76), where the floor has no collision: the
	// player is held in the air (linked to a script origin) looking down at them
	holder = Spawn("script_origin", (5850, -1800, 60));
	player PlayerLinkToAbsolute(holder);
	views = [];
	views[views.size] = (16, -63, 0);
	views[views.size] = (25, -80, 0);
	views[views.size] = (12, -45, 0);
	for (round = 0; round < 3; round++)
	{
		for (i = 0; i < views.size; i++)
		{
			player SetPlayerAngles(views[i]);
			wait 2;
			step("grass view " + i + " at " + player.origin + " angles " + player GetPlayerAngles());
		}
		holder MoveTo(holder.origin + (100, -150, 0), 1);
		wait 1.5;
	}
	step("done");
}
