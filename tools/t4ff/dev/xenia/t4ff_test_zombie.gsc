// t4ff test: look at a zombie up close with streaming on and off (the fastfile's textures alone,
// then the PC originals streamed). Every step sets t4ff_test ("dvar set t4ff_test ..." in the log).
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

	step("vote");
	player notify("menuresponse", "ugxm_vote_host", "cl");
	wait 0.5;
	player notify("menuresponse", "ugxm_vote_host", "start_all");
	wait 8;
	player EnableInvulnerability();
	player.ignoreme = true;

	step("waiting for a zombie");
	zombie = undefined;
	for (i = 0; i < 120 && !IsDefined(zombie); i++)
	{
		zombies = GetAiSpeciesArray("axis", "all");
		for (j = 0; j < zombies.size; j++)
		{
			if (IsAlive(zombies[j]))
			{
				zombie = zombies[j];
				break;
			}
		}
		wait 1;
	}
	if (!IsDefined(zombie))
	{
		step("no zombie");
		return;
	}

	// wait until it has climbed in, then stand in front of it
	wait 12;
	for (k = 0; k < 3; k++)
	{
		if (!IsAlive(zombie))
			break;
		forward = AnglesToForward(zombie.angles);
		player SetOrigin(zombie.origin + forward * 45);
		eye = player.origin + (0, 0, 60);
		player SetPlayerAngles(VectorToAngles((zombie.origin + (0, 0, 45)) - eye));
		wait 1.5;
	}
	step("zombie close");
	wait 2;
	ExecuteCommand("r_stream 0");
	wait 2;
	step("zombie streaming off");
	wait 2;
	ExecuteCommand("r_stream 1");
	wait 5;
	step("zombie streaming on");
	wait 2;
	step("done");
}
