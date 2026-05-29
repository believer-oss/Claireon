// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

namespace ClaireonToolCatalogAbbreviations
{
	/**
	 * Abbreviation / synonym table consumed at both index-build time and query
	 * time by the tool catalog matcher. Keys are common abbreviations or
	 * alternate terms; values are space-separated expansions.
	 *
	 * Single source of truth: the previous Python-side mirror of this table
	 * was removed when the wire format was simplified to send raw fields
	 * only.
	 */
	struct FEntry
	{
		const TCHAR* Key;
		const TCHAR* Expansion;
	};

	inline const TArray<FEntry>& GetTable()
	{
		static const TArray<FEntry> Table = {
			{ TEXT("bp"),             TEXT("blueprint") },
			{ TEXT("bt"),             TEXT("behavior tree behaviortree") },
			{ TEXT("st"),             TEXT("state tree statetree") },
			{ TEXT("dt"),             TEXT("data table datatable") },
			{ TEXT("pie"),            TEXT("play in editor playtest runtime") },
			{ TEXT("fx"),             TEXT("effects niagara particles vfx visual") },
			{ TEXT("vfx"),            TEXT("effects niagara particles visual") },
			{ TEXT("eqs"),            TEXT("environment query system") },
			{ TEXT("scs"),            TEXT("simple construction script components") },
			{ TEXT("ai"),             TEXT("artificial intelligence behavior tree") },
			{ TEXT("ui"),             TEXT("widget user interface umg hud") },
			{ TEXT("umg"),            TEXT("widget user interface") },
			{ TEXT("hud"),            TEXT("widget user interface display") },
			{ TEXT("csv"),            TEXT("comma separated export import datatable") },
			{ TEXT("perf"),           TEXT("performance trace profiling") },
			{ TEXT("prof"),           TEXT("performance trace profiling") },
			{ TEXT("hitch"),          TEXT("performance trace frame spike") },
			{ TEXT("compile"),        TEXT("build compilation") },
			{ TEXT("diff"),           TEXT("compare comparison difference") },
			{ TEXT("spawn"),          TEXT("create instantiate enemy") },
			{ TEXT("dmg"),            TEXT("damage health combat") },
			{ TEXT("anim"),           TEXT("animation") },
			{ TEXT("prop"),           TEXT("property properties") },
			{ TEXT("ref"),            TEXT("reference dependency referencers") },
			{ TEXT("cmd"),            TEXT("command console commandlet") },
			{ TEXT("log"),            TEXT("logging output tail") },
			{ TEXT("map"),            TEXT("level world open") },
			{ TEXT("lvl"),            TEXT("level map world") },
			{ TEXT("test"),           TEXT("automation testing") },
			{ TEXT("asset"),          TEXT("content resource") },
			{ TEXT("graph"),          TEXT("blueprint node visual script") },
			{ TEXT("node"),           TEXT("blueprint graph") },
			{ TEXT("pin"),            TEXT("blueprint connection") },
			{ TEXT("var"),            TEXT("variable property") },
			{ TEXT("func"),           TEXT("function method") },
			{ TEXT("comp"),           TEXT("component") },
			{ TEXT("actor"),          TEXT("entity object level") },
			{ TEXT("pawn"),           TEXT("character player") },
			{ TEXT("ability"),        TEXT("gameplay gas") },
			{ TEXT("gas"),            TEXT("gameplay ability system") },
			{ TEXT("redirect"),       TEXT("redirector fixup") },
			{ TEXT("resave"),         TEXT("save serialize") },
			{ TEXT("cook"),           TEXT("package deploy") },
			{ TEXT("screenshot"),     TEXT("screenshot capture image snap viewport") },
			{ TEXT("fly"),            TEXT("flythrough camera") },
			{ TEXT("cam"),            TEXT("camera flythrough viewport") },
			{ TEXT("camera"),         TEXT("camera camera_asset camera_node rig") },
			{ TEXT("cam_asset"),      TEXT("camera asset rig camera_asset") },
			{ TEXT("camera_node"),    TEXT("camera_node node camera rig") },
			{ TEXT("rig"),            TEXT("camera rig camera_asset") },
			{ TEXT("bb"),             TEXT("blackboard behavior tree keys") },
			{ TEXT("niag"),           TEXT("niagara") },
			{ TEXT("prefab"),         TEXT("prefabrication level instance") },
			{ TEXT("row"),            TEXT("datatable entry record") },
			{ TEXT("col"),            TEXT("column field property datatable") },
			{ TEXT("validate"),       TEXT("check verify integrity") },
			{ TEXT("inspect"),        TEXT("read view examine structure") },
			{ TEXT("edit"),           TEXT("modify change update session") },
			{ TEXT("search"),         TEXT("find query lookup discover") },
			{ TEXT("list"),           TEXT("enumerate show available") },
			{ TEXT("get"),            TEXT("read fetch retrieve") },
			{ TEXT("set"),            TEXT("write update modify") },
			{ TEXT("load"),           TEXT("open read import") },
			{ TEXT("save"),           TEXT("write export persist") },
			{ TEXT("open"),           TEXT("open map level world load launch") },
			{ TEXT("find"),           TEXT("search discover lookup asset find_assets") },
			{ TEXT("create"),         TEXT("create new add spawn place") },
			{ TEXT("delete"),         TEXT("delete remove destroy") },
			{ TEXT("run"),            TEXT("run execute start launch") },
			{ TEXT("stop"),           TEXT("stop end close terminate kill") },
			{ TEXT("undo"),           TEXT("undo transaction revert rollback") },
			{ TEXT("redo"),           TEXT("redo transaction reapply") },
			{ TEXT("foliage"),        TEXT("foliage paint vegetation instanced") },
			{ TEXT("landscape"),      TEXT("landscape terrain heightmap sculpt paint") },
			{ TEXT("spline"),         TEXT("spline path curve control point") },
			{ TEXT("pcg"),            TEXT("procedural content generation graph") },
			{ TEXT("pcg_graph"),      TEXT("pcg procedural content generation graph") },
			{ TEXT("niagara"),        TEXT("niagara vfx particles effect emitter") },
			{ TEXT("statetree"),      TEXT("state tree statetree hierarchical") },
			{ TEXT("material"),       TEXT("material shader expression parameter") },
			{ TEXT("animbp"),         TEXT("animation blueprint animgraph anim_graph anim animbp") },
			{ TEXT("widgetbp"),       TEXT("widget blueprint umg widgetbp user interface") },
			{ TEXT("apply_spec"),     TEXT("apply spec declarative batch idempotent") },
			{ TEXT("sequence"),       TEXT("sequence level_sequence sequencer cinematic keyframe track") },
			{ TEXT("level_sequence"), TEXT("sequence level_sequence sequencer cinematic keyframe track") },
			{ TEXT("chooser"),        TEXT("chooser table result row column context parameter fallback") },
			{ TEXT("proxytable"),     TEXT("proxy table inherit entry lookup") },
			{ TEXT("proxyasset"),     TEXT("proxy asset type result context parameter") },
			{ TEXT("context"),        TEXT("context parameter data input output struct class") },
			{ TEXT("inherit"),        TEXT("inherit parent proxy table chain") },
			{ TEXT("fallback"),       TEXT("fallback default result") },
			{ TEXT("soundcue"),       TEXT("sound cue node graph audio wave_player mixer") },
			{ TEXT("metasound"),      TEXT("metasound source builder audio document graph node") },
			{ TEXT("soundclass"),     TEXT("sound class hierarchy children audio mix volume pitch") },
			{ TEXT("soundmix"),       TEXT("sound mix adjuster envelope audio fade volume") },
			{ TEXT("attenuation"),    TEXT("attenuation falloff audio spatial 3d distance") },
			{ TEXT("concurrency"),    TEXT("concurrency limit voice stealing audio resolution") },
		};
		return Table;
	}
}
