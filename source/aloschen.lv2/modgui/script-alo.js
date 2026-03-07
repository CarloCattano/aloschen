function (event, funcs) {
    var RINGS_LS_KEY = 'aloschen.show_rings';

    function clamp01(v) { return (v > -Infinity) ? Math.max(0, Math.min(1, v)) : 0; }
    function clampBars(bars) { return (bars > 0) ? Math.max(1, Math.min(16, Math.round(bars))) : 1; }

    function safeGetLocalStorage(key) {
        try { return (window && window.localStorage) ? window.localStorage.getItem(key) : null; } catch (e) { return null; }
    }

    function safeSetLocalStorage(key, value) {
        try { if (window && window.localStorage) window.localStorage.setItem(key, value); } catch (e) {}
    }

    function ensureRings(icon, data) {
        if (data.rings_ready) return;

        var rings = ['cycle', 'host', 'bars', 'bars-active', 'steps', 'steps-active'];
        rings.forEach(function(name) {
            data['ring_' + name.replace('-', '_')] = icon.find('.alo-ring-' + name);
        });
        data.step_dots = icon.find('.alo-step-dots');

        // Radii must match icon-alo.html
        data.ring_cycle_c = 2 * Math.PI * 26;
        data.ring_host_c = 2 * Math.PI * 14;
        data.ring_bars_c = 2 * Math.PI * 46;
        data.ring_steps_c = 2 * Math.PI * 38;

        data.ring_cycle.css({ 'stroke-dasharray': data.ring_cycle_c, 'stroke-dashoffset': data.ring_cycle_c });
        data.ring_host.css({ 'stroke-dasharray': data.ring_host_c, 'stroke-dashoffset': data.ring_host_c });
        data.ring_bars.css({ 'stroke-dasharray': '0', 'stroke-dashoffset': '0' });
        data.ring_bars_active.css({ 'stroke-dasharray': '0', 'stroke-dashoffset': '0' });
        data.ring_steps.css({ 'stroke-dasharray': '0', 'stroke-dashoffset': '0' });
        data.ring_steps_active.css({ 'stroke-dasharray': '0', 'stroke-dashoffset': '0' });

        ensureRingToggle(icon, data);
        data.rings_ready = true;
    }

    function rebuildStepDots(icon, data) {
        if (!data || !data.step_dots || !data.step_dots.length) return;
        var steps = Math.max(1, Math.min(64, data.steps || 4));

        if (data.stepDotsCount === steps && data.step_dots.children().length === steps) return;

        var html = '';
        for (var i = 0; i < steps; ++i) {
            var a = (i / steps) * (Math.PI * 2.0) - (Math.PI / 2.0);
            var x = 50 + 38 * Math.cos(a);
            var y = 50 + 38 * Math.sin(a);
            html += '<circle class="alo-step-dot" data-step="' + i + '" cx="' + x.toFixed(3) + '" cy="' + y.toFixed(3) + '" r="1.45"></circle>';
        }
        data.step_dots.empty().append(html);
        data.stepDotsCount = steps;
    }

    function setActiveStepDot(icon, data) {
        if (!data || !data.step_dots) return;
        data.step_dots.find('.alo-step-dot.alo-active').removeClass('alo-active');
        if (data.bar_step >= 0 && data.bar_step < (data.steps || 0)) {
            data.step_dots.find('.alo-step-dot[data-step="' + data.bar_step + '"]').addClass('alo-active');
        }
    }

    function setRingsVisible(icon, data, visible) {
        data.showRings = !!visible;
        var root = icon.closest('.mod-pedal').length ? icon.closest('.mod-pedal') : icon;
        
        root.toggleClass('alo-rings-off', !data.showRings);
        if (icon !== root) icon.toggleClass('alo-rings-off', !data.showRings);

        if (data.ring_toggle_img) {
            data.ring_toggle_img.removeClass('on off').addClass(data.showRings ? 'on' : 'off');
        }
        safeSetLocalStorage(RINGS_LS_KEY, data.showRings ? '1' : '0');
    }

    function ensureRingToggle(icon, data) {
        if (data.ring_toggle_ready) return;

        data.ring_toggle = icon.find('.alo-ring-toggle');
        data.ring_toggle_img = icon.find('.alo-ring-toggle-image');

        if (!data.ring_toggle.length) {
            setRingsVisible(icon, data, true);
        } else {
            setRingsVisible(icon, data, safeGetLocalStorage(RINGS_LS_KEY) !== '0');
            data.ring_toggle.off('click.aloRings').on('click.aloRings', function (e) {
                if (e) { e.preventDefault(); e.stopPropagation(); }
                setRingsVisible(icon, data, !data.showRings);
                return false;
            });
        }
        data.ring_toggle_ready = true;
    }

    function setRing(el, circ, phase01) {
        if (el && el.length && circ > 0) {
            el.css('stroke-dashoffset', String((1.0 - clamp01(phase01)) * circ));
        }
    }

    /* blink helper (matches C UI 250ms cycle) */
    function blinkOn() {
        return ((Math.floor(Date.now() / 250) % 2) === 0);
    }

    function computeRingColour(data) {
        if (!data || !data.loopStates) return 'gray';
        var s = data.loopStates;
        var anyRec = (s.loop1_state >= 0.75) || (s.loop2_state >= 0.75) || (s.loop3_state >= 0.75);
        var anyPlay = (s.loop1_state >= 0.5) || (s.loop2_state >= 0.5) || (s.loop3_state >= 0.5);
        var anyArm = (s.loop1_state >= 0.25) || (s.loop2_state >= 0.25) || (s.loop3_state >= 0.25);
        if (anyRec) return 'red';
        if (anyPlay) return '#00cc00';
        if (anyArm) {
            return blinkOn() ? 'orange' : 'gray';
        }
        return 'gray';
    }

    function updateBarsRing(icon, data) {
        if (!data || !data.ring_bars || !(data.ring_bars_c > 0)) return;
        var bars = Math.max(1, data.bars || 1);
        var seg = data.ring_bars_c / bars;
        var onLen = seg * 0.78;

        var colour = computeRingColour(data);
        data.ring_bars.css({
            'stroke': colour,
            'stroke-dasharray': onLen + ' ' + (seg - onLen),
            'stroke-dashoffset': '0'
        });

        var barIndex = (data.bar_step >= 0) ? Math.max(0, Math.min(bars - 1, Math.floor(data.bar_step / 4))) : 0;
        data.ring_bars_active.css({
            'stroke': colour,
            'stroke-dasharray': onLen + ' ' + (data.ring_bars_c - onLen),
            'stroke-dashoffset': -barIndex * seg
        });
    }

    function updateStepsRing(icon, data) {
        if (!data || !data.ring_steps || !(data.ring_steps_c > 0)) return;
        var steps = Math.max(1, Math.min(64, data.steps || 4));
        var seg = data.ring_steps_c / steps;
        var onLen = seg * 0.55;

        data.ring_steps.css({ 'stroke-dasharray': onLen + ' ' + (seg - onLen), 'stroke-dashoffset': '0' });

        var stepIndex = (data.bar_step >= 0 && data.bar_step < steps) ? data.bar_step : 0;
        data.ring_steps_active.css({ 'stroke-dasharray': onLen + ' ' + (data.ring_steps_c - onLen), 'stroke-dashoffset': -stepIndex * seg });
    }

    function updateCycleRing(icon, data) {
        if (!data || !data.ring_cycle || !(data.ring_cycle_c > 0)) return;
        var colour = computeRingColour(data);
        data.ring_cycle.css('stroke', colour);
        if (typeof data.cycle_phase === 'number') {
            setRing(data.ring_cycle, data.ring_cycle_c, data.cycle_phase);
        }
    }

    function setAnyRecordingClass(icon, data) {
        var s = data.loopStates || { loop1_state: 0, loop2_state: 0, loop3_state: 0 };
        data.loopStates = s;
        var any = (s.loop1_state >= 0.75) || (s.loop2_state >= 0.75) || (s.loop3_state >= 0.75);

        if (any !== data.anyRecording) {
            data.anyRecording = any;
            icon.toggleClass('alo-recording', any);
        }
    }

    function setLoopLight(icon, stateSymbol, value) {
        var cls = value >= 0.75 ? 'alo-rec' : value >= 0.45 ? 'alo-playing' : value >= 0.20 ? 'alo-armed' : 'alo-off';
        icon.find('.alo-loop-light[data-state-symbol="' + stateSymbol + '"]')
            .removeClass('alo-off alo-armed alo-playing alo-rec').addClass(cls);
    }

    function setUndoLight(icon, stateSymbol, value) {
        icon.find('.alo-undo-light[data-state-symbol="' + stateSymbol + '"]')
            .removeClass('alo-off alo-queued').addClass(value >= 0.20 ? 'alo-queued' : 'alo-off');
    }

    function setUndoEnabled(icon, stateSymbol, enabled) {
        icon.find('.alo-undo-light[data-state-symbol="' + stateSymbol + '"]')
            .closest('.mod-green-light').toggleClass('alo-undo-disabled', !enabled);
    }

    function rebuildStepbar(icon, steps) {
        var bar = icon.find('.alo-stepbar');
        if (!bar.length) return;
        var html = '';
        for (var i = 0; i < steps; ++i) html += '<div class="alo-step" data-step="' + i + '"></div>';
        bar.empty().append(html).attr('data-steps', steps);
    }

    function setActiveStep(icon, active) {
        var bar = icon.find('.alo-stepbar');
        if (!bar.length) return;
        bar.find('.alo-step.alo-active').removeClass('alo-active');
        if (active >= 0) bar.find('.alo-step[data-step="' + active + '"]').addClass('alo-active');
    }

    function updateAllStepsAndRings(icon, data) {
        rebuildStepDots(icon, data);
        updateBarsRing(icon, data);
        updateStepsRing(icon, data);
        updateCycleRing(icon, data);
        setActiveStep(icon, (data.bar_step >= 0 && data.bar_step < data.steps) ? data.bar_step : -1);
        setActiveStepDot(icon, data);
    }

    function handle(symbol, value, icon, data) {
        if (!symbol || typeof symbol !== 'string') return;
        var match;

        if (symbol === 'bars' || symbol === 'bar_step') {
            if (symbol === 'bars') {
                data.bars = clampBars(value);
                data.steps = data.bars * 4;
                rebuildStepbar(icon, data.steps);
            } else {
                data.bar_step = Math.round(value);
            }
            updateAllStepsAndRings(icon, data);
        } else if (symbol === 'cycle_phase') {
            data.cycle_phase = value;
            updateCycleRing(icon, data);
        } else if (symbol === 'host_bar_phase') {
            setRing(data.ring_host, data.ring_host_c, value);
        } else if ((match = symbol.match(/^loop(\d)_vol$/))) {
            data[symbol] = value;
        } else if ((match = symbol.match(/^loop(\d)_state$/))) {
            data.loopStates = data.loopStates || { loop1_state: 0, loop2_state: 0, loop3_state: 0 };
            data.loopStates[symbol] = value;
            setLoopLight(icon, symbol, value);
            setAnyRecordingClass(icon, data);
            updateBarsRing(icon, data);
            updateCycleRing(icon, data);
        } else if ((match = symbol.match(/^loop(\d)_has_audio$/))) {
            setUndoEnabled(icon, 'undo' + match[1] + '_state', value >= 0.5);
        } else if ((match = symbol.match(/^undo(\d)_state$/))) {
            setUndoLight(icon, symbol, value);
        }
    }

    function setPortValue(icon, symbol, value) {
        if (!symbol) return;
        if (event.api_version >= 1 && funcs && funcs.set_port_value) {
            funcs.set_port_value(symbol, value);
        } else {
            var ctrl = icon.find('[mod-role="input-control-port"][mod-port-symbol="' + symbol + '"]');
            if (ctrl.length) ctrl.controlWidget('setValue', value);
        }
    }

    if (!event || !event.icon) return;
    var icon = event.icon;
    var data = icon.data('alo') || { bars: 2, steps: 8, bar_step: 0 };
    ensureRings(icon, data);

    if (event.type === 'start') {
        if (event.ports) {
            for (var p in event.ports) {
                if (event.ports.hasOwnProperty(p) && event.ports[p]) {
                    handle(event.ports[p].symbol, event.ports[p].value, icon, data);
                }
            }
        }

        var mute = icon.find('.alo-mute-all');
        if (mute.length) {
            mute.off('click.aloMute').on('click.aloMute', function (e) {
                if (e) { e.preventDefault(); e.stopPropagation(); }
                var d = icon.data('alo') || {};
                var isOn = !!d.muteIsOn;

                if (!isOn) {
                    d.muteSaved = {
                        v1: (d.loop1_vol > -Infinity) ? d.loop1_vol : 1.0,
                        v2: (d.loop2_vol > -Infinity) ? d.loop2_vol : 1.0,
                        v3: (d.loop3_vol > -Infinity) ? d.loop3_vol : 1.0
                    };
                }

                var targetVols = isOn && d.muteSaved ? d.muteSaved : { v1: 0, v2: 0, v3: 0 };

                [1, 2, 3].forEach(function(i) {
                    setPortValue(icon, 'loop' + i + '_vol', targetVols['v' + i]);
                    d['loop' + i + '_vol'] = targetVols['v' + i];
                });

                d.muteIsOn = !isOn;
                mute.find('.mod-switch-image').removeClass('on off').addClass(d.muteIsOn ? 'on' : 'off');
                icon.data('alo', d);
                return false;
            });
        }

        setAnyRecordingClass(icon, data);
        updateAllStepsAndRings(icon, data);
        if (!icon.find('.alo-stepbar .alo-step').length) {
            rebuildStepbar(icon, data.steps);
            setActiveStep(icon, (data.bar_step >= 0 && data.bar_step < data.steps) ? data.bar_step : -1);
        }
    } else if (event.type === 'change') {
        handle(event.symbol, event.value, icon, data);
    }

    icon.data('alo', data);
}
