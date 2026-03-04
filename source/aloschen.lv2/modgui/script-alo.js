function (event, funcs) {
    var RINGS_LS_KEY = 'aloschen.show_rings';

    function clamp01(v) {
        if (!(v > -Infinity)) return 0;
        if (v < 0) return 0;
        if (v > 1) return 1;
        return v;
    }

    function clampBars(bars) {
        if (!(bars > 0)) return 1;
        if (bars < 1) return 1;
        if (bars > 32) return 32;
        return Math.round(bars);
    }

    function ensureRings(icon, data) {
        if (data.rings_ready) return;

        data.ring_cycle = icon.find('.alo-ring-cycle');
        data.ring_host = icon.find('.alo-ring-host');
        data.ring_bars = icon.find('.alo-ring-bars');
        data.ring_bars_active = icon.find('.alo-ring-bars-active');
        data.ring_steps = icon.find('.alo-ring-steps');
        data.ring_steps_active = icon.find('.alo-ring-steps-active');
        data.step_dots = icon.find('.alo-step-dots');

        // Radii must match icon-alo.html
        data.ring_cycle_c = 2 * Math.PI * 26;
        data.ring_host_c = 2 * Math.PI * 14;
        data.ring_bars_c = 2 * Math.PI * 46;
        data.ring_steps_c = 2 * Math.PI * 38;

        if (data.ring_cycle && data.ring_cycle.length) {
            data.ring_cycle.css('stroke-dasharray', String(data.ring_cycle_c));
            data.ring_cycle.css('stroke-dashoffset', String(data.ring_cycle_c));
        }
        if (data.ring_host && data.ring_host.length) {
            data.ring_host.css('stroke-dasharray', String(data.ring_host_c));
            data.ring_host.css('stroke-dashoffset', String(data.ring_host_c));
        }

        // Bars ring dash is configured when we know Bars.
        if (data.ring_bars && data.ring_bars.length) {
            data.ring_bars.css('stroke-dasharray', '0');
            data.ring_bars.css('stroke-dashoffset', '0');
        }
        if (data.ring_bars_active && data.ring_bars_active.length) {
            data.ring_bars_active.css('stroke-dasharray', '0');
            data.ring_bars_active.css('stroke-dashoffset', '0');
        }

        if (data.ring_steps && data.ring_steps.length) {
            data.ring_steps.css('stroke-dasharray', '0');
            data.ring_steps.css('stroke-dashoffset', '0');
        }
        if (data.ring_steps_active && data.ring_steps_active.length) {
            data.ring_steps_active.css('stroke-dasharray', '0');
            data.ring_steps_active.css('stroke-dashoffset', '0');
        }

        ensureRingToggle(icon, data);

        data.rings_ready = true;
    }

    function rebuildStepDots(icon, data) {
        if (!data || !data.step_dots || !data.step_dots.length) return;

        var steps = data.steps || 4;
        if (steps < 1) steps = 1;
        if (steps > 128) steps = 128;

        if (data.stepDotsCount === steps && data.step_dots.children().length === steps) {
            return;
        }

        // SVG coords: viewBox 0..100, center 50,50
        var cx = 50;
        var cy = 50;
        var radius = 38; // align with steps ring
        var dotR = 1.45;

        var html = '';
        for (var i = 0; i < steps; ++i) {
            var a = (i / steps) * (Math.PI * 2.0) - (Math.PI / 2.0);
            var x = cx + radius * Math.cos(a);
            var y = cy + radius * Math.sin(a);
            html += '<circle class="alo-step-dot" data-step="' + i + '" cx="' + x.toFixed(3) + '" cy="' + y.toFixed(3) + '" r="' + dotR + '"></circle>';
        }

        data.step_dots.empty();
        data.step_dots.append(html);
        data.stepDotsCount = steps;
    }

    function setActiveStepDot(icon, data) {
        if (!data || !data.step_dots || !data.step_dots.length) return;
        if (!(data.steps > 0)) return;

        var s = data.bar_step;
        if (!(s >= 0 && s < data.steps)) {
            data.step_dots.find('.alo-step-dot.alo-active').removeClass('alo-active');
            return;
        }

        data.step_dots.find('.alo-step-dot.alo-active').removeClass('alo-active');
        data.step_dots.find('.alo-step-dot[data-step="' + s + '"]').addClass('alo-active');
    }

    function safeGetLocalStorage(key) {
        try {
            if (!window || !window.localStorage) return null;
            return window.localStorage.getItem(key);
        } catch (e) {
            return null;
        }
    }

    function safeSetLocalStorage(key, value) {
        try {
            if (!window || !window.localStorage) return;
            window.localStorage.setItem(key, value);
        } catch (e) {
            /* ignore */
        }
    }

    function setRingsVisible(icon, data, visible) {
        data.showRings = !!visible;
        if (data.showRings) icon.removeClass('alo-rings-off');
        else icon.addClass('alo-rings-off');

        if (data.ring_toggle_img && data.ring_toggle_img.length) {
            data.ring_toggle_img.removeClass('on off');
            data.ring_toggle_img.addClass(data.showRings ? 'on' : 'off');
        }

        safeSetLocalStorage(RINGS_LS_KEY, data.showRings ? '1' : '0');
    }

    function ensureRingToggle(icon, data) {
        if (data.ring_toggle_ready) return;

        data.ring_toggle = icon.find('.alo-ring-toggle');
        data.ring_toggle_img = icon.find('.alo-ring-toggle-image');

        var stored = safeGetLocalStorage(RINGS_LS_KEY);
        var visible = true;
        if (stored === '0') visible = false;

        setRingsVisible(icon, data, visible);

        if (data.ring_toggle && data.ring_toggle.length) {
            data.ring_toggle.off('click.aloRings');
            data.ring_toggle.on('click.aloRings', function (e) {
                if (e) {
                    if (e.preventDefault) e.preventDefault();
                    if (e.stopPropagation) e.stopPropagation();
                }
                setRingsVisible(icon, data, !data.showRings);
                return false;
            });
        }

        data.ring_toggle_ready = true;
    }

    function setRing(el, circ, phase01) {
        if (!el || !el.length || !(circ > 0)) return;
        var p = clamp01(phase01);
        var off = (1.0 - p) * circ;
        el.css('stroke-dashoffset', String(off));
    }

    function updateBarsRing(icon, data) {
        if (!data) return;
        if (!data.ring_bars || !data.ring_bars.length) return;
        if (!data.ring_bars_active || !data.ring_bars_active.length) return;
        if (!(data.ring_bars_c > 0)) return;

        var bars = data.bars || 1;
        if (bars < 1) bars = 1;

        var seg = data.ring_bars_c / bars;
        var onLen = seg * 0.78;
        var offLen = seg - onLen;

        // Base ring: repeating dashes for each bar.
        data.ring_bars.css('stroke-dasharray', String(onLen) + ' ' + String(offLen));
        data.ring_bars.css('stroke-dashoffset', '0');

        // Active bar: a single dash, positioned via dashoffset.
        var barIndex = 0;
        if (data.bar_step >= 0) {
            barIndex = Math.floor(data.bar_step / 4);
            if (barIndex < 0) barIndex = 0;
            if (barIndex >= bars) barIndex = bars - 1;
        }

        data.ring_bars_active.css('stroke-dasharray', String(onLen) + ' ' + String(data.ring_bars_c - onLen));
        data.ring_bars_active.css('stroke-dashoffset', String(-barIndex * seg));
    }

    function updateStepsRing(icon, data) {
        if (!data) return;
        if (!data.ring_steps || !data.ring_steps.length) return;
        if (!data.ring_steps_active || !data.ring_steps_active.length) return;
        if (!(data.ring_steps_c > 0)) return;

        var steps = data.steps || 4;
        if (steps < 1) steps = 1;
        if (steps > 128) steps = 128;

        var seg = data.ring_steps_c / steps;
        var onLen = seg * 0.55;
        var offLen = seg - onLen;

        // Base ring: repeating dashes for each step.
        data.ring_steps.css('stroke-dasharray', String(onLen) + ' ' + String(offLen));
        data.ring_steps.css('stroke-dashoffset', '0');

        // Active step: a single dash, positioned via dashoffset.
        var stepIndex = data.bar_step;
        if (!(stepIndex >= 0 && stepIndex < steps)) {
            stepIndex = 0;
        }

        data.ring_steps_active.css('stroke-dasharray', String(onLen) + ' ' + String(data.ring_steps_c - onLen));
        data.ring_steps_active.css('stroke-dashoffset', String(-stepIndex * seg));
    }

    function setAnyRecordingClass(icon, data) {
        if (!icon || !data) return;

        var s = data.loopStates;
        if (!s) {
            s = { loop1_state: 0, loop2_state: 0, loop3_state: 0 };
            data.loopStates = s;
        }

        var any = (s.loop1_state >= 0.75) || (s.loop2_state >= 0.75) || (s.loop3_state >= 0.75);
        if (any === data.anyRecording) return;

        data.anyRecording = any;
        if (any) icon.addClass('alo-recording');
        else icon.removeClass('alo-recording');
    }

    function setLoopLight(icon, stateSymbol, value) {
        var sel = '.alo-loop-light[data-state-symbol="' + stateSymbol + '"]';
        var el = icon.find(sel);
        if (!el.length) {
            return;
        }

        el.removeClass('alo-off alo-armed alo-playing alo-rec');

        // Match the native UI semantics:
        // - idle: 0
        // - armed (waiting): ~0.25
        // - playing (has loop): ~0.5
        // - recording: 1
        if (value >= 0.75) {
            el.addClass('alo-rec');
        } else if (value >= 0.45) {
            el.addClass('alo-playing');
        } else if (value >= 0.20) {
            el.addClass('alo-armed');
        } else {
            el.addClass('alo-off');
        }
    }

    function setUndoLight(icon, stateSymbol, value) {
        var sel = '.alo-undo-light[data-state-symbol="' + stateSymbol + '"]';
        var el = icon.find(sel);
        if (!el.length) {
            return;
        }

        el.removeClass('alo-off alo-queued');
        if (value >= 0.20) {
            el.addClass('alo-queued');
        } else {
            el.addClass('alo-off');
        }
    }

    function setUndoEnabled(icon, stateSymbol, enabled) {
        var sel = '.alo-undo-light[data-state-symbol="' + stateSymbol + '"]';
        var el = icon.find(sel);
        if (!el.length) {
            return;
        }
        var wrap = el.closest('.mod-green-light');
        if (!wrap.length) {
            return;
        }
        if (enabled) {
            wrap.removeClass('alo-undo-disabled');
        } else {
            wrap.addClass('alo-undo-disabled');
        }
    }

    function rebuildStepbar(icon, steps) {
        var bar = icon.find('.alo-stepbar');
        if (!bar.length) {
            return;
        }

        bar.empty();
        for (var i = 0; i < steps; ++i) {
            bar.append('<div class="alo-step" data-step="' + i + '"></div>');
        }

        bar.attr('data-steps', String(steps));
    }

    function setActiveStep(icon, active) {
        var bar = icon.find('.alo-stepbar');
        if (!bar.length) {
            return;
        }

        bar.find('.alo-step.alo-active').removeClass('alo-active');
        if (active >= 0) {
            bar.find('.alo-step[data-step="' + active + '"]').addClass('alo-active');
        }
    }

    function handle(symbol, value) {
        if (!event || !event.icon) {
            return;
        }

        // MOD can occasionally emit placeholder/malformed events.
        // Ignore empty/unknown symbols to avoid console noise.
        if (!symbol || typeof symbol !== 'string') {
            return;
        }

        var icon = event.icon;
        var data = icon.data('alo') || { bars: 2, steps: 8, bar_step: 0 };
        ensureRings(icon, data);

        if (symbol === 'bars') {
            data.bars = clampBars(value);
            data.steps = data.bars * 4;
            rebuildStepbar(icon, data.steps);
            rebuildStepDots(icon, data);
            updateBarsRing(icon, data);
            updateStepsRing(icon, data);
            // Re-apply active step after rebuild
            if (!(data.bar_step >= 0 && data.bar_step < data.steps)) {
                setActiveStep(icon, -1);
            } else {
                setActiveStep(icon, data.bar_step);
            }
            setActiveStepDot(icon, data);
        } else if (symbol === 'bar_step') {
            data.bar_step = Math.round(value);
            rebuildStepDots(icon, data);
            updateBarsRing(icon, data);
            updateStepsRing(icon, data);
            if (!(data.bar_step >= 0 && data.bar_step < data.steps)) {
                setActiveStep(icon, -1);
            } else {
                setActiveStep(icon, data.bar_step);
            }
            setActiveStepDot(icon, data);
        } else if (symbol === 'cycle_phase') {
            setRing(data.ring_cycle, data.ring_cycle_c, value);
        } else if (symbol === 'host_bar_phase') {
            setRing(data.ring_host, data.ring_host_c, value);
        } else if (symbol === 'loop1_state') {
            data.loopStates = data.loopStates || { loop1_state: 0, loop2_state: 0, loop3_state: 0 };
            data.loopStates.loop1_state = value;
            setLoopLight(icon, 'loop1_state', value);
            setAnyRecordingClass(icon, data);
        } else if (symbol === 'loop2_state') {
            data.loopStates = data.loopStates || { loop1_state: 0, loop2_state: 0, loop3_state: 0 };
            data.loopStates.loop2_state = value;
            setLoopLight(icon, 'loop2_state', value);
            setAnyRecordingClass(icon, data);
        } else if (symbol === 'loop3_state') {
            data.loopStates = data.loopStates || { loop1_state: 0, loop2_state: 0, loop3_state: 0 };
            data.loopStates.loop3_state = value;
            setLoopLight(icon, 'loop3_state', value);
            setAnyRecordingClass(icon, data);
        } else if (symbol === 'loop1_has_audio') {
            setUndoEnabled(icon, 'undo1_state', value >= 0.5);
        } else if (symbol === 'loop2_has_audio') {
            setUndoEnabled(icon, 'undo2_state', value >= 0.5);
        } else if (symbol === 'loop3_has_audio') {
            setUndoEnabled(icon, 'undo3_state', value >= 0.5);
        } else if (symbol === 'undo1_state') {
            setUndoLight(icon, 'undo1_state', value);
        } else if (symbol === 'undo2_state') {
            setUndoLight(icon, 'undo2_state', value);
        } else if (symbol === 'undo3_state') {
            setUndoLight(icon, 'undo3_state', value);
        }

        icon.data('alo', data);
    }

    if (event.type === 'start') {
        var ports = event.ports;
        if (ports) {
            for (var p in ports) {
                if (!ports.hasOwnProperty(p)) continue;

                var port = ports[p];
                if (!port) continue;
                handle(port.symbol, port.value);
            }
        }

        // Ensure stepbar exists even if we didn't receive bars yet.
        // (Some hosts may not send initial values for every port.)
        var icon = event.icon;
        var data = icon.data('alo') || { bars: 2, steps: 8, bar_step: 0 };
        ensureRings(icon, data);
        setAnyRecordingClass(icon, data);
        rebuildStepDots(icon, data);
        setActiveStepDot(icon, data);
        updateBarsRing(icon, data);
        updateStepsRing(icon, data);
        if (!icon.find('.alo-stepbar .alo-step').length) {
            rebuildStepbar(icon, data.steps);
            setActiveStep(icon, (data.bar_step >= 0 && data.bar_step < data.steps) ? data.bar_step : -1);
        }
        icon.data('alo', data);
    } else if (event.type === 'change') {
        handle(event.symbol, event.value);
    }
}
