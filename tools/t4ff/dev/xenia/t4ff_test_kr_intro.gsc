// t4ff test: Kino Rezurrection (map "d") from the start. Logs where the player is through the
// helicopter intro (about 27 seconds linked to the helicopter, then dropped on the ground), then
// makes the player invulnerable and logs the zombies of the first round.
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
	for (t = 0; t < 60; t += 3)
	{
		step("t " + t + " at " + player.origin + " angles " + player GetPlayerAngles() + " weapon " + player GetCurrentWeapon());
		wait 3;
	}
	for (t = 60; t < 150; t += 10)
	{
		zombies = GetAiSpeciesArray("axis", "all");
		step("t " + t + " at " + player.origin + " zombies " + zombies.size + " round " + level.round_number);
		wait 10;
	}
	step("done");
}
