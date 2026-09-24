/*
	Chaos Mode: a random event every N seconds. Timed events undo themselves. Props, effects
	and AI that chaos spawns live in the "chaos" and "chaos_ai" pools (util.gsc): each pool is
	capped, entries expire on their own, and turning Chaos Mode off wipes both at once.
*/

#include maps\_utility;
#include common_scripts\utility;
#include maps\mod_menu\util;

chaos_event_count()
{
	return 22;
}

chaos_set(on)
{
	level notify("mm_stop_chaos");
	self mm_onoff("Chaos Mode", on);
	if (!on)
	{
		wiped = chaos_wipe();
		if (wiped > 0)
			self iprintln("Wiped ^3" + wiped + "^7 chaos spawns");
		return;
	}

	level endon("mm_stop_chaos");
	mm_announce("^1CHAOS MODE ^7ENGAGED");
	count = chaos_event_count();
	last = -1;
	for (;;)
	{
		wait mm_get("chaos_interval", "level", 20);
		index = randomInt(count);
		if (index == last)
			index = (index + 1) % count;
		last = index;
		self thread chaos_event(index);
	}
}

chaos_wipe()
{
	return mm_pool_clear("chaos") + mm_pool_clear("chaos_ai");
}

chaos_interval_set(value)
{
	self iprintln("Chaos every ^3" + value + "^7 seconds");
}

chaos_event(index)
{
	self endon("disconnect");
	switch (index)
	{
	case 0:
		mm_announce("^5CHAOS: ^7Moon gravity");
		mm_set_dvar("g_gravity", "120");
		mm_set_dvar("phys_gravity", "-150");
		wait 15;
		mm_set_dvar("g_gravity", mm_get("gravity", "level", 800));
		mm_restore_dvar("phys_gravity", "-800");
		break;
	case 1:
		mm_announce("^5CHAOS: ^7Bullet time");
		setTimeScale(0.4);
		wait 5;
		setTimeScale(mm_get("timescale", "level", 1));
		break;
	case 2:
		mm_announce("^5CHAOS: ^7Fast forward");
		setTimeScale(1.8);
		wait 8;
		setTimeScale(mm_get("timescale", "level", 1));
		break;
	case 3:
		visions = maps\mod_menu\world::vision_names();
		mm_announce("^5CHAOS: ^7Everything looks wrong");
		visionSetNaked(visions[randomIntRange(1, visions.size)], 1);
		wait 15;
		visionSetNaked(mm_level_vision(), 2);
		break;
	case 4:
		self maps\mod_menu\fun::meteor_shower();
		break;
	case 5:
		self maps\mod_menu\fun::rapture();
		break;
	case 6:
		mm_announce("^5CHAOS: ^7Everybody gets gibbed");
		self maps\mod_menu\ai::gib_all();
		break;
	case 7:
		mm_announce("^5CHAOS: ^7Earthquake");
		earthquake(0.6, 8, self.origin, 100000);
		break;
	case 8:
		self maps\mod_menu\fun::blood_rain();
		break;
	case 9:
		mm_announce("^5CHAOS: ^7Disco inferno");
		self thread maps\mod_menu\fun::disco_set(true);
		wait 12;
		level notify("mm_stop_disco");
		resetSunLight();
		visionSetNaked(mm_level_vision(), 1);
		break;
	case 10:
		mm_announce("^5CHAOS: ^7Weapon roulette");
		players = get_players();
		for (i = 0; i < players.size; i++)
			players[i] maps\mod_menu\weapons::give_random_weapon();
		break;
	case 11:
		mm_announce("^5CHAOS: ^7Sonic speed");
		players = get_players();
		for (i = 0; i < players.size; i++)
			players[i] setMoveSpeedScale(3);
		wait 12;
		for (i = 0; i < players.size; i++)
		{
			if (isDefined(players[i]))
				players[i] setMoveSpeedScale(players[i] mm_get("speed", "self", 1));
		}
		break;
	case 12:
		mm_announce("^5CHAOS: ^7Silent film");
		setDvar("sf_use_chaplin", 1);
		wait 15;
		setDvar("sf_use_chaplin", 0);
		break;
	case 13:
		mm_announce("^5CHAOS: ^7Hurricane");
		mm_set_dvar("wind_global_vector", "3000 -2500 0");
		wait 15;
		mm_restore_dvar("wind_global_vector", "0 0 0");
		break;
	case 14:
		mm_announce("^5CHAOS: ^7Upside-down physics");
		mm_set_dvar("phys_gravity", "500");
		physicsExplosionSphere(self.origin, 2000, 100, 1);
		wait 12;
		mm_restore_dvar("phys_gravity", "-800");
		break;
	case 15:
		mm_announce("^5CHAOS: ^7Enemies go flying");
		self maps\mod_menu\ai::launch_all();
		break;
	case 16:
		mm_announce("^5CHAOS: ^7It's raining props");
		self chaos_prop_rain(16);
		break;
	case 17:
		mm_announce("^5CHAOS: ^7Special effects budget");
		self chaos_fx_storm(10);
		break;
	case 18:
		if (mm_is_zombies())
			mm_announce("^5CHAOS: ^7Zombie ambush");
		else
			mm_announce("^5CHAOS: ^7Ambush");
		self chaos_ambush(4);
		break;
	case 19:
		if (mm_is_zombies())
			mm_announce("^5CHAOS: ^7It's raining zombies");
		else
			mm_announce("^5CHAOS: ^7Paratroopers");
		self chaos_ai_rain(4, false);
		break;
	case 20:
		mm_announce("^5CHAOS: ^7Prop tornado");
		self chaos_tornado(10);
		break;
	default:
		if (mm_is_zombies())
			mm_announce("^5CHAOS: ^7Zombie artillery");
		else
			mm_announce("^5CHAOS: ^7Human artillery");
		self chaos_ai_rain(4, true);
		break;
	}
}

chaos_player()
{
	players = get_players();
	return players[randomInt(players.size)];
}

chaos_prop(model, pos)
{
	prop = spawn("script_model", pos);
	prop setModel(model);
	prop.angles = (randomInt(360), randomInt(360), 0);
	mm_pool_add("chaos", prop, 14, 40);
	return prop;
}

// Random models drop out of the sky around random players and stay for a few seconds.
chaos_prop_rain(count)
{
	models = maps\mod_menu\forge::model_list();
	if (models.size == 0)
		return;
	for (i = 0; i < count; i++)
	{
		player = chaos_player();
		spot = mm_spot_near(player, 64, 450);
		top = mm_ceiling(spot + (0, 0, 32), 700) - (0, 0, 40);
		prop = chaos_prop(models[randomInt(models.size)], top);
		self thread chaos_prop_fall(prop);
		wait 0.15;
	}
}

chaos_prop_fall(prop)
{
	trace = mm_fly(prop, (randomIntRange(-60, 60), randomIntRange(-60, 60), -50), 3, prop, (200, 300, 0));
	if (isDefined(prop) && isDefined(trace))
	{
		self maps\mod_menu\shots::shot_hit(trace, (0, 0, -400));
		if (modelHasPhysPreset(prop.model))
			prop physicsLaunch(prop.origin, (randomIntRange(-20, 20), randomIntRange(-20, 20), 10));
	}
}

// Random loaded effects around random players, wiped after 8 seconds (looping ones included).
chaos_fx_storm(count)
{
	keys = maps\mod_menu\fun::fx_keys();
	if (keys.size == 0)
		return;
	for (i = 0; i < count; i++)
	{
		spot = mm_spot_near(chaos_player(), 96, 500);
		thread mm_pool_fx("chaos", level._effect[keys[randomInt(keys.size)]], spot + (0, 0, 8), 8, 40);
		wait 0.3;
	}
}

// Enemies (zombies on zombies maps) spawn around random players. Survivors are removed after
// 30 seconds so a zombies round cannot stall on them.
chaos_ambush(count)
{
	for (i = 0; i < count; i++)
	{
		spot = mm_spot_near(chaos_player(), 200, 450);
		guy = maps\mod_menu\ai::spawn_enemy_at(spot);
		if (!isDefined(guy))
			return; // AI limit reached
		mm_pool_add_ai("chaos_ai", guy, 30, 6);
		mm_blood(guy.origin + (0, 0, 40));
		wait 0.3;
	}
}

// Enemies drop out of the sky. explode: they are fired at the players and blow up on impact.
chaos_ai_rain(count, explode)
{
	for (i = 0; i < count; i++)
	{
		player = chaos_player();
		spot = mm_spot_near(player, 150, 400);
		guy = maps\mod_menu\ai::spawn_enemy_at(spot);
		if (!isDefined(guy))
			return;
		mm_pool_add_ai("chaos_ai", guy, 30, 6);

		top = mm_ceiling(spot + (0, 0, 32), 600) - (0, 0, 80);
		velocity = (randomIntRange(-80, 80), randomIntRange(-80, 80), -100);
		if (explode)
		{
			aim = player.origin + (randomIntRange(-150, 150), randomIntRange(-150, 150), 0);
			velocity = vectorNormalize(aim - top) * 900;
		}
		self thread maps\mod_menu\shots::ai_shot(guy, top, velocity, explode);
		wait 0.4;
	}
}

// Random models orbit a player, widening and rising, then get flung outwards.
chaos_tornado(count)
{
	models = maps\mod_menu\forge::model_list();
	if (models.size == 0)
		return;
	player = chaos_player();
	props = [];
	for (i = 0; i < count; i++)
		props[i] = chaos_prop(models[randomInt(models.size)], player.origin + (0, 0, 40));

	center = player.origin;
	for (t = 0; t < 80; t++)
	{
		if (isDefined(player) && isAlive(player))
			center = player.origin;
		radius = 90 + t * 2;
		for (i = 0; i < props.size; i++)
		{
			if (!isDefined(props[i]))
				continue;
			yaw = t * 14 + i * (360 / props.size);
			height = 30 + (i % 4) * 30 + t * 1.5;
			props[i] moveTo(center + (cos(yaw) * radius, sin(yaw) * radius, height), 0.1);
			props[i] rotateYaw(40, 0.1);
		}
		if (t % 10 == 0)
		{
			ai = mm_ai_near(center, radius + 40);
			for (i = 0; i < ai.size; i++)
			{
				if (ai[i].team != "allies" && distance(ai[i].origin, center) > radius - 60)
					thread mm_fling(ai[i], vectorNormalize(mm_flat(ai[i].origin - center)) * 500 + (0, 0, 400), self);
			}
		}
		wait 0.1;
	}

	for (i = 0; i < props.size; i++)
	{
		if (isDefined(props[i]))
			props[i] moveGravity(vectorNormalize(mm_flat(props[i].origin - center)) * 700 + (0, 0, 300), 3);
	}
}
