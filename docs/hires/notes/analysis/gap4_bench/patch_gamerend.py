import sys
p='/home/simonea/ultima7_exult/tmp/gap4/exult-src/gamerend.cc'
s=open(p).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    assert n==count, (old[:60], n)
    s=s.replace(old,new)
rep('#include "perf.h"\n', '#include "perf.h"\n#include "bench_hooks.h"\nRendPhases g_phase;\n')
# paint_map phases
rep('''	int cx;
	int cy;    // Chunk #'s.
	// Paint all the flat scenery.
	for (cy = start_chunky; cy != stop_chunky; cy = INCR_CHUNK(cy)) {
		const int yoff = Figure_screen_offset(cy, scrollty) - gwin->get_scrollty_lo();
		for (cx = start_chunkx; cx != stop_chunkx; cx = INCR_CHUNK(cx)) {
			const int xoff = Figure_screen_offset(cx, scrolltx) - gwin->get_scrolltx_lo();
			paint_chunk_flats(cx, cy, xoff, yoff);
		}
	}
''','''	int cx;
	int cy;    // Chunk #'s.
	auto bt0 = RendPhases::clk::now();
	// Paint all the flat scenery.
	for (cy = start_chunky; cy != stop_chunky; cy = INCR_CHUNK(cy)) {
		const int yoff = Figure_screen_offset(cy, scrollty) - gwin->get_scrollty_lo();
		for (cx = start_chunkx; cx != stop_chunkx; cx = INCR_CHUNK(cx)) {
			const int xoff = Figure_screen_offset(cx, scrolltx) - gwin->get_scrolltx_lo();
			paint_chunk_flats(cx, cy, xoff, yoff);
			g_phase.n_chunks++;
		}
	}
	auto bt1 = RendPhases::clk::now();
	g_phase.t_flats += RendPhases::ms(bt0, bt1);
''')
rep('''			paint_chunk_flat_rles(cx, cy, xoff, yoff);
		}
	}
''','''			paint_chunk_flat_rles(cx, cy, xoff, yoff);
		}
	}
	auto bt2 = RendPhases::clk::now();
	g_phase.t_flat_rles += RendPhases::ms(bt1, bt2);
''')
rep('''	/// Dungeon Blackness (but disable in map editor mode)
	if''','''	auto bt3 = RendPhases::clk::now();
	g_phase.t_objects += RendPhases::ms(bt2, bt3);
	/// Dungeon Blackness (but disable in map editor mode)
	if''')
rep('''	// Outline selected objects.
	const Game_object_shared_vector& sel ''','''	auto bt4 = RendPhases::clk::now();
	g_phase.t_blackness += RendPhases::ms(bt3, bt4);
	// Outline selected objects.
	const Game_object_shared_vector& sel ''')
rep('''			Paint_selected_chunks(gwin, sman->get_xform(13), start_chunkx, start_chunky, stop_chunkx, stop_chunky);
		}
	}
	return light_sources;''','''			Paint_selected_chunks(gwin, sman->get_xform(13), start_chunkx, start_chunky, stop_chunkx, stop_chunky);
		}
	}
	g_phase.t_select += RendPhases::ms(bt4, RendPhases::clk::now());
	return light_sources;''')
# Game_window::paint phases
rep('''	int light_sources = 0;

	if (main_actor) {
		light_sources = render->paint_map(gx, gy, gw, gh);
	} else {
		win->fill8(0);
	}

	effects->paint();    // Draw sprites.
''','''	int light_sources = 0;
	auto pt0 = RendPhases::clk::now();

	if (main_actor) {
		light_sources = render->paint_map(gx, gy, gw, gh);
	} else {
		win->fill8(0);
	}
	auto pt1 = RendPhases::clk::now();
	g_phase.t_paint_map += RendPhases::ms(pt0, pt1);

	effects->paint();    // Draw sprites.
	auto pt2 = RendPhases::clk::now();
	g_phase.t_effects += RendPhases::ms(pt1, pt2);
''')
rep('''	gump_man->paint(false);
	if (dragging) {
		dragging->paint();    // Paint what user is dragging.
	}
	effects->paint_text();
	gump_man->paint(true);
''','''	auto pt3 = RendPhases::clk::now();
	g_phase.t_border += RendPhases::ms(pt2, pt3);
	gump_man->paint(false);
	if (dragging) {
		dragging->paint();    // Paint what user is dragging.
	}
	effects->paint_text();
	gump_man->paint(true);
	auto pt4 = RendPhases::clk::now();
	g_phase.t_gumps += RendPhases::ms(pt3, pt4);
''')
rep('''		clock->set_light_source(carried_light + light_sources, in_dungeon);
	}

	win->EndPaintIntoGuardBand();
	win->clear_clip();
}''','''		clock->set_light_source(carried_light + light_sources, in_dungeon);
	}
	auto pt5 = RendPhases::clk::now();
	g_phase.t_lights += RendPhases::ms(pt4, pt5);
	g_phase.t_total += RendPhases::ms(pt0, pt5);
	g_phase.n_paints++;

	win->EndPaintIntoGuardBand();
	win->clear_clip();
}''')
rep('''	while ((obj = next.get_next()) != nullptr) {
		obj->paint();
	}
}''','''	while ((obj = next.get_next()) != nullptr) {
		obj->paint();
		g_phase.n_flat_objs++;
	}
}''')
rep('''	obj->paint();    // Finally, paint this one.''','''	obj->paint();    // Finally, paint this one.
	g_phase.n_objs++;''')
rep('''				light_sources += get_light_strength(light_obj, main_actor);''','''				light_sources += get_light_strength(light_obj, main_actor);
				g_phase.n_lights++;''')
open(p,'w').write(s)
print("ok")
