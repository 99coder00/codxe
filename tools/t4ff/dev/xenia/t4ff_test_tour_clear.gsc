// t4ff test: tour Kino Der Toten's streamed world surfaces and props without anyone at the
// controller. Every step sets t4ff_test, which the Xenia log shows as "dvar set t4ff_test ...";
// CoD Xe logs each highmip file it serves.
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

	points = [];
	points[points.size] = (76, 2076, 3960);
	points[points.size] = (1509, 1450, 3960);
	points[points.size] = (287, 1423, 3960);
	points[points.size] = (-1071, -704, 4090);
	points[points.size] = (-279, -960, 4140);
	points[points.size] = (-744, 1088, 4400);
	points[points.size] = (1600, 572, 4376);
	points[points.size] = (-1302, 758, 4440);
	points[points.size] = (-508, 582, 3950);
	points[points.size] = (1000, 2080, 4000);
	for (i = 0; i < points.size; i++)
	{
		step("tour " + i + " at " + points[i]);
		player SetOrigin(points[i]);
		for (yaw = 0; yaw < 360; yaw += 90)
		{
			player SetPlayerAngles((10, yaw, 0));
			wait 1.5;
		}
	}
	// streaming off drops every streamed level (the revert path) and keeps them dropped; back on,
	// they load again (internal dvars: a console command, not SetDvar)
	ExecuteCommand("r_stream 0");
	wait 3;
	step("streaming off");
	wait 2;
	ExecuteCommand("r_stream 1");
	wait 8;
	step("done");
}
