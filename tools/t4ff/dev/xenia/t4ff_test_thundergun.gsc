// t4ff test: fire the Thundergun on Kino Der Toten without anyone at the controller.
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
	wait 5;

	// UGX's vote: Classic, then start
	step("vote");
	player notify("menuresponse", "ugxm_vote_host", "cl");
	wait 0.5;
	player notify("menuresponse", "ugxm_vote_host", "start_all");
	wait 8;
	player EnableInvulnerability();

	// the Thundergun with UGX's handler on, as zombies come
	SetDvar("t4ff_tg", "on");
	for (i = 0; i < 4; i++)
		fire(player, "thunderg");
	for (i = 0; i < 4; i++)
		fire(player, "thunderg_upgraded");
	step("done");
}

fire(player, weapon)
{
	if (!player HasWeapon(weapon))
	{
		player GiveWeapon(weapon);
		player SwitchToWeapon(weapon);
		step("raising " + weapon);
		wait 3;
	}
	player GiveMaxAmmo(weapon);
	zombies = GetAiSpeciesArray("axis", "all");
	step("firing " + weapon + " with " + zombies.size + " zombies, handler " + GetDvar("t4ff_tg"));
	ExecuteCommand("+attack");
	wait 0.3;
	ExecuteCommand("-attack");
	wait 3;
	step("fired " + weapon);
}
