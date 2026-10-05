// t4ff test: Kino Rezurrection (map "d"), its wall buys' chalk outlines (512x256 decals): after the
// intro, stands at the Stakeout, M14 and MP40 buys (their triggers, from the map entities) and turns
// slowly, so the top levels have time to stream in. Each view sets t4ff_test.
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
	wait 42;
	step("landed at " + player.origin);
	// in front of the chalk decals (their world surfaces' centres, from the converted zone; the
	// other sides are off the map) and of one of the map's chalk maps (theater_chalkmap_c)
	names = [];
	spots = [];
	yaws = [];
	// the Stakeout chalk faces -y: where a player stands to buy it (61 units), and closer, on the
	// floor under them
	names[names.size] = "stakeout";
	spots[spots.size] = floor_at((-1045, -403 - 61, 200));
	yaws[yaws.size] = 90;
	names[names.size] = "stakeout close";
	spots[spots.size] = floor_at((-1045, -403 - 30, 200));
	yaws[yaws.size] = 90;
	names[names.size] = "m14";
	spots[spots.size] = (-376, 89 - 70, 157 - 60);
	yaws[yaws.size] = 90;
	names[names.size] = "m14";
	spots[spots.size] = (-376, 89 + 70, 157 - 60);
	yaws[yaws.size] = 270;
	names[names.size] = "mp40";
	spots[spots.size] = (-2063 + 70, -916, -114 - 60);
	yaws[yaws.size] = 180;
	names[names.size] = "chalkmap";
	spots[spots.size] = (-74, 209 - 110, 196 - 60);
	yaws[yaws.size] = 90;
	names[names.size] = "chalkmap";
	spots[spots.size] = (-74, 209 + 110, 196 - 60);
	yaws[yaws.size] = 270;
	for (i = 0; i < spots.size; i++)
	{
		player SetOrigin(spots[i]);
		player SetPlayerAngles((0, yaws[i], 0));
		wait 1;
		step("chalk " + names[i] + " first at " + player.origin + " yaw " + yaws[i]);
		wait 5;
		step("chalk " + names[i] + " settled at " + player.origin + " yaw " + yaws[i]);
	}
	step("done");
}

// the floor under a point (a trace down), or the point
floor_at(point)
{
	trace = BulletTrace(point, point - (0, 0, 600), false, undefined);
	step("floor under " + point + ": " + trace["position"] + " fraction " + trace["fraction"]);
	if (trace["fraction"] < 1)
		return trace["position"] + (0, 0, 2);
	return point;
}
