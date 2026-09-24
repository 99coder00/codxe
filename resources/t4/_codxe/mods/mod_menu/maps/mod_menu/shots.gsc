/*
	Projectiles: models and freshly spawned enemies fired along a ballistic arc. Used by the
	Model Gun (Forge), the Prop/Zombie Cannon bullet modes and Chaos Mode. Every shot is pooled
	(mm_pool_add in util.gsc), so rapid fire recycles the oldest shots instead of running the
	level out of entities or AI slots.
*/

#include maps\_utility;
#include common_scripts\utility;
#include maps\mod_menu\util;

shot_speed()
{
	return self mm_get("shot_speed", "self", 1400);
}

shot_speed_set(value)
{
	self iprintln("Projectile speed ^3" + value);
}

// Knocks over whatever a shot hit. Friendlies are left alone: killing one fails the mission.
shot_hit(trace, velocity)
{
	hit = trace["entity"];
	if (isDefined(hit) && isAI(hit) && isAlive(hit) && hit.team != "allies")
		thread mm_fling(hit, velocity * 0.4 + (0, 0, 250), self);
	physicsExplosionSphere(trace["position"], 96, 24, 1);
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

// style: "Bounce" (lands, tumbles if the model has physics, removed after 10s), "Build" (turns
// into a Forge prop where it lands), "Explode".
model_shot(model, start, velocity, style)
{
	prop = spawn("script_model", start);
	prop setModel(model);
	prop.angles = vectorToAngles(velocity);
	mm_pool_add("shots", prop, 10, 24);

	spin = (randomIntRange(-360, 360), randomIntRange(-360, 360), 0);
	trace = mm_fly(prop, velocity, 3, prop, spin);
	if (!isDefined(prop))
		return;

	pos = prop.origin;
	if (isDefined(trace))
		self shot_hit(trace, velocity);

	if (style == "Explode")
	{
		prop delete();
		mm_explode(pos, 200, self);
		return;
	}

	if (style == "Build" && isDefined(trace))
	{
		yaw = prop.angles[1];
		if (self maps\mod_menu\forge::spawn_model_at(model, pos, (0, yaw, 0)))
		{
			prop delete();
			return;
		}
	}

	if (isDefined(trace) && modelHasPhysPreset(model))
		prop physicsLaunch(pos, velocity * 0.03);
}

// ---------------------------------------------------------------------------
// Enemies
// ---------------------------------------------------------------------------

// Spawns an enemy (a zombie on zombies maps) in front of the player and fires it. explode:
// it blows up on impact; otherwise it lands alive and gets back to work.
fire_ai(explode)
{
	self endon("disconnect");
	if (isDefined(self.mm_ai_shot_next) && self.mm_ai_shot_next > getTime())
		return;
	self.mm_ai_shot_next = getTime() + 400;

	view = self getPlayerAngles();
	spot = mm_ground(self.origin + anglesToForward((0, view[1], 0)) * 48 + (0, 0, 24));
	guy = maps\mod_menu\ai::spawn_enemy_at(spot);
	if (!isDefined(guy))
	{
		self iprintln("^1Spawn failed (AI limit reached?)");
		return;
	}
	// A zombies round only ends once every zombie is dead, so on zombies maps a cannon zombie that
	// lands somewhere unreachable is removed after 45 seconds.
	lifetime = 0;
	if (mm_is_zombies())
		lifetime = 45;
	mm_pool_add_ai("shot_ai", guy, lifetime, 6);

	dir = self mm_forward();
	self ai_shot(guy, self getEye() + dir * 64 - (0, 0, 30), dir * self shot_speed(), explode);
}

// Flies guy on a tag_origin mover. The mover is pooled with a short lifetime, so even if this
// thread dies mid-flight the reaper unlinks guy and deletes the mover.
ai_shot(guy, start, velocity, explode)
{
	mover = mm_spawn_mover(guy.origin, guy.angles);
	mover.mm_rider = guy;
	mm_pool_add("movers", mover, 6, 12);
	guy unlink();
	guy linkTo(mover);
	mover moveTo(start, 0.05);
	wait 0.05;

	trace = mm_fly(mover, velocity, 3, guy, (0, 540, 0));
	pos = start;
	if (isDefined(mover))
		pos = mover.origin;
	if (isDefined(guy))
		guy unlink();
	if (isDefined(mover))
		mover delete();
	if (!isAlive(guy))
		return;

	if (explode)
	{
		thread mm_fling(guy, vectorNormalize(velocity) * 200 + (0, 0, 300), self);
		mm_explode(pos, 220, self);
		return;
	}

	landing = pos;
	if (isDefined(trace))
	{
		self shot_hit(trace, velocity);
		landing = trace["position"] + trace["normal"] * 24;
	}
	guy teleport(mm_ground(landing + (0, 0, 16)));
	earthquake(0.3, 0.6, landing, 500);
}
