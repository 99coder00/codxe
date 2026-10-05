// t4ff test: Kino Rezurrection (map "d"). Waits through the helicopter intro (the player is
// linked to it for about 27 seconds), then tours the map's rooms, four views each, invulnerable.
// Every step sets t4ff_test, which the Xenia log shows as "dvar set t4ff_test ...".
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
	for (t = 0; t < 42; t += 6)
	{
		step("intro " + t + " at " + player.origin);
		wait 6;
	}
	step("landed at " + player.origin + " weapon " + player GetCurrentWeapon());
	wait 2;

	// floor level next to the map's perk machines, boxes and rooms (from its map entities)
	points = [];
	points[points.size] = (22, 892, -10);       // player start
	points[points.size] = (-581, 860, -55);     // Quick Revive
	points[points.size] = (-73, 300, 130);      // box 1, the lobby's balcony
	points[points.size] = (580, 120, -55);      // Deadshot
	points[points.size] = (1267, 170, -40);     // box 8
	points[points.size] = (1469, -660, -110);   // box 7
	points[points.size] = (1286, -1500, 40);    // box 2
	points[points.size] = (814, -1850, -160);   // Electric Cherry
	points[points.size] = (223, -1620, -150);   // teleporter pad
	points[points.size] = (-85, -2300, -150);   // box 3
	points[points.size] = (-147, -600, -155);   // box 9, the theatre
	points[points.size] = (-1459, -1800, -150); // box 4
	points[points.size] = (-1399, -580, -160);  // random perk
	points[points.size] = (-1038, 140, 190);    // box 6
	points[points.size] = (-64, -10, 180);      // Pack-a-Punch
	for (i = 0; i < points.size; i++)
	{
		player SetOrigin(points[i]);
		for (yaw = 0; yaw < 360; yaw += 90)
		{
			player SetPlayerAngles((5, yaw, 0));
			wait 1.5;
			step("tour " + i + " yaw " + yaw + " at " + player.origin);
		}
	}
	zombies = GetAiSpeciesArray("axis", "all");
	step("zombies " + zombies.size + " round " + level.round_number);
	step("done");
}
