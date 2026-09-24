/*
	Forge: spawn and move props. The model list is discovered at runtime from script_models the
	map already placed plus weapon world models, so every entry is guaranteed to be loaded.

	"Solid" spawns use SpawnCollision(), a GSC function CoD Xe r347+ adds to T4 singleplayer.
	On an older CoD Xe build the name is unknown and every level fails to load.
*/

#include maps\_utility;
#include common_scripts\utility;
#include maps\mod_menu\util;

model_list()
{
	if (isDefined(level.mm_models))
		return level.mm_models;

	models = [];
	ents = getEntArray("script_model", "classname");
	for (i = 0; i < ents.size && models.size < 50; i++)
	{
		if (isDefined(ents[i].model) && ents[i].model != "tag_origin" && ents[i].model != "")
			models = mm_array_add_unique(models, ents[i].model);
	}

	weapons = maps\mod_menu\weapons::weapon_pool();
	for (i = 0; i < weapons.size && models.size < 60; i++)
		models = mm_array_add_unique(models, getWeaponModel(weapons[i]));

	level.mm_models = models;
	return models;
}

// Props are capped across all players: each one is a game entity, and a level that runs out
// of entities ends with a fatal error.
forge_cap()
{
	return 64;
}

forge_total()
{
	total = 0;
	players = get_players();
	for (p = 0; p < players.size; p++)
	{
		list = players[p].mm_forge;
		if (!isDefined(list))
			continue;
		for (i = 0; i < list.size; i++)
		{
			if (isDefined(list[i]))
				total++;
		}
	}
	return total;
}

// Keeps self.mm_forge free of props that were deleted some other way.
forge_live()
{
	list = [];
	if (!isDefined(self.mm_forge))
		return list;
	for (i = 0; i < self.mm_forge.size; i++)
	{
		if (isDefined(self.mm_forge[i]))
			list[list.size] = self.mm_forge[i];
	}
	return list;
}

forge_track(ent)
{
	list = self forge_live();
	list[list.size] = ent;
	self.mm_forge = list;
}

spawn_model(model)
{
	trace = self mm_trace();
	view = self getPlayerAngles();
	if (self spawn_model_at(model, trace["position"], (0, view[1], 0)))
		self iprintln("Spawned ^3" + model);
}

spawn_random_model()
{
	models = model_list();
	if (models.size == 0)
	{
		self iprintln("^1No models found on this map");
		return;
	}
	self spawn_model(models[randomInt(models.size)]);
}

// Places a Forge prop, honouring the Solid and Physics toggles. Returns false at the prop cap.
spawn_model_at(model, pos, angles)
{
	if (forge_total() >= forge_cap())
	{
		if (!isDefined(self.mm_cap_warned) || self.mm_cap_warned < getTime())
		{
			self.mm_cap_warned = getTime() + 2000;
			self iprintln("^1Prop limit (" + forge_cap() + ") reached - Undo or Clear first");
		}
		return false;
	}

	if (self mm_get("forge_solid", "self", false))
		ent = spawnCollision(model, "mm_forge", pos, angles);
	else
	{
		ent = spawn("script_model", pos);
		ent setModel(model);
		ent.angles = angles;
	}

	self forge_track(ent);
	self.mm_forge_model = model;

	if (self mm_get("forge_physics", "self", false) && modelHasPhysPreset(model))
		ent physicsLaunch(ent.origin, (0, 0, -10));
	return true;
}

solid_set(on)
{
	self mm_onoff("Solid Spawns (CoD Xe SpawnCollision)", on);
}

physics_set(on)
{
	self mm_onoff("Physics Spawns", on);
}

undo_spawn()
{
	list = self forge_live();
	if (list.size == 0)
	{
		self iprintln("Nothing to undo");
		return;
	}
	last = list.size - 1;
	list[last] delete();
	list[last] = undefined;
	self.mm_forge = list;
	self iprintln("Removed last spawn");
}

clear_spawns()
{
	list = self forge_live();
	count = list.size;
	for (i = 0; i < count; i++)
		list[i] delete();
	self.mm_forge = [];
	self iprintln("Deleted ^3" + count + "^7 props");
}

aimed_entity()
{
	trace = self mm_trace();
	ent = trace["entity"];
	if (!isDefined(ent) || isPlayer(ent))
		return undefined;
	return ent;
}

delete_aimed()
{
	ent = self aimed_entity();
	if (!isDefined(ent))
	{
		self iprintln("^1Aim at a prop, door or AI");
		return;
	}
	if (isAI(ent))
		mm_kill(ent, self);
	else
		ent delete();
	self iprintln("Deleted");
}

launch_aimed()
{
	ent = self aimed_entity();
	if (!isDefined(ent))
	{
		self iprintln("^1Aim at something first");
		return;
	}
	if (isAI(ent))
	{
		thread mm_fling(ent, self mm_forward() * 900 + (0, 0, 400), self);
		return;
	}
	if (isDefined(ent.model) && modelHasPhysPreset(ent.model))
		ent physicsLaunch(ent.origin, self mm_forward() * 60 + (0, 0, 20));
	else
		ent moveGravity(self mm_forward() * 900 + (0, 0, 500), 4);
}

// Turns the prop you are aiming at by 45 degrees.
rotate_aimed()
{
	ent = self aimed_entity();
	if (!isDefined(ent) || isAI(ent))
	{
		self iprintln("^1Aim at a prop first");
		return;
	}
	ent rotateYaw(45, 0.25);
}

// Spawns another copy of the model you are aiming at.
clone_aimed()
{
	ent = self aimed_entity();
	if (!isDefined(ent) || isAI(ent) || ent.classname != "script_model" || !isDefined(ent.model) || ent.model == "")
	{
		self iprintln("^1Aim at a model first");
		return;
	}
	self spawn_model(ent.model);
}

// Ring of eight copies of the last spawned model around you.
model_ring()
{
	model = self.mm_forge_model;
	if (!isDefined(model))
	{
		models = model_list();
		if (models.size == 0)
		{
			self iprintln("^1No models found on this map");
			return;
		}
		model = models[randomInt(models.size)];
	}

	placed = 0;
	for (i = 0; i < 8; i++)
	{
		yaw = i * 45;
		pos = mm_ground(self.origin + (cos(yaw) * 140, sin(yaw) * 140, 48));
		if (!self spawn_model_at(model, pos, (0, yaw + 90, 0)))
			break;
		placed++;
	}
	self iprintln("Placed ^3" + placed + "^7 x " + model);
}

// Random models fall around your crosshair. They are temporary (see shots.gsc).
model_rain()
{
	self endon("disconnect");
	models = model_list();
	if (models.size == 0)
	{
		self iprintln("^1No models found on this map");
		return;
	}
	self iprintln("^3Incoming props");
	center = self mm_aim_pos();
	for (i = 0; i < 14; i++)
	{
		spot = center + (randomFloatRange(-250, 250), randomFloatRange(-250, 250), 32);
		top = mm_ceiling(spot, 700) - (0, 0, 40);
		self thread maps\mod_menu\shots::model_shot(models[randomInt(models.size)], top, (0, 0, -100), "Bounce");
		wait 0.12;
	}
}

// ---------------------------------------------------------------------------
// Model Gun: every shot also fires a model
// ---------------------------------------------------------------------------

model_gun_set(on)
{
	self notify("mm_stop_model_gun");
	self mm_onoff("Model Gun", on);
	if (!on)
		return;
	if (model_list().size == 0)
	{
		self iprintln("^1No models found on this map");
		return;
	}

	self endon("mm_stop_model_gun");
	self endon("disconnect");
	for (;;)
	{
		self waittill("weapon_fired");
		if (isDefined(self.mm_model_gun_next) && self.mm_model_gun_next > getTime())
			continue;
		self.mm_model_gun_next = getTime() + 150;
		self thread model_gun_fire();
	}
}

model_gun_fire()
{
	self endon("disconnect");
	count = self mm_get("model_gun_count", "self", 1);
	style = self model_gun_style();
	speed = self maps\mod_menu\shots::shot_speed();
	for (i = 0; i < count; i++)
	{
		dir = self mm_forward();
		if (count > 1)
			dir = vectorNormalize(dir + mm_random_vec(0.12));
		self thread maps\mod_menu\shots::model_shot(self gun_model(), self getEye() + dir * 60, dir * speed, style);
	}
}

gun_model()
{
	if (isDefined(self.mm_gun_model))
		return self.mm_gun_model;
	models = model_list();
	return models[randomInt(models.size)];
}

gun_model_pick(model)
{
	if (model == "random")
	{
		self.mm_gun_model = undefined;
		self iprintln("Model Gun: ^3random models");
		return;
	}
	self.mm_gun_model = model;
	self iprintln("Model Gun: ^3" + model);
}

model_gun_style_names()
{
	names = [];
	names[names.size] = "Bounce";
	names[names.size] = "Build";
	names[names.size] = "Explode";
	return names;
}

model_gun_style_set(index)
{
	names = model_gun_style_names();
	self.mm_model_gun_style = names[index];
	if (index == 1)
		self iprintln("Model Gun: shots become props (Undo/Clear removes them)");
	else
		self iprintln("Model Gun style: ^3" + names[index]);
}

model_gun_style()
{
	if (isDefined(self.mm_model_gun_style))
		return self.mm_model_gun_style;
	return "Bounce";
}

model_gun_count_set(value)
{
	self iprintln("Models per shot: ^3" + value);
}

// Hold LB to grab the prop you are aiming at, RB to spin it, release LB to throw it.
pickup_set(on)
{
	self notify("mm_stop_pickup");
	self mm_onoff("Grab Mode (hold LB)", on);
	if (!on)
	{
		self enableOffhandWeapons();
		return;
	}

	self endon("mm_stop_pickup");
	self endon("disconnect");
	for (;;)
	{
		wait 0.05;
		if (!isAlive(self) || self.mm_open)
			continue;
		self disableOffhandWeapons();
		if (!self secondaryOffhandButtonPressed())
			continue;
		ent = self aimed_entity();
		if (isDefined(ent) && !isAI(ent))
			self carry(ent);
	}
}

carry(ent)
{
	dist = distance(self getEye(), ent.origin);
	dist = mm_clamp(dist, 80, 400);
	last = ent.origin;
	while (isDefined(ent) && isAlive(self) && self secondaryOffhandButtonPressed() && !self.mm_open)
	{
		last = ent.origin;
		ent moveTo(self getEye() + self mm_forward() * dist, 0.05);
		if (self fragButtonPressed())
			ent rotateYaw(30, 0.05);
		wait 0.05;
	}
	if (!isDefined(ent))
		return;

	fling = (ent.origin - last) * 20;
	if (isDefined(ent.model) && modelHasPhysPreset(ent.model))
		ent physicsLaunch(ent.origin, fling * 0.05);
}
