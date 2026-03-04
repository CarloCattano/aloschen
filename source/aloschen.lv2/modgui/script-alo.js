function (event, funcs) {
    function clampBars(bars) {
        if (!(bars > 0)) return 1;
        if (bars < 1) return 1;
        if (bars > 32) return 32;
        return Math.round(bars);
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

        if (symbol === 'bars') {
            data.bars = clampBars(value);
            data.steps = data.bars * 4;
            rebuildStepbar(icon, data.steps);
            // Re-apply active step after rebuild
            if (!(data.bar_step >= 0 && data.bar_step < data.steps)) {
                setActiveStep(icon, -1);
            } else {
                setActiveStep(icon, data.bar_step);
            }
        } else if (symbol === 'bar_step') {
            data.bar_step = Math.round(value);
            if (!(data.bar_step >= 0 && data.bar_step < data.steps)) {
                setActiveStep(icon, -1);
            } else {
                setActiveStep(icon, data.bar_step);
            }
        } else if (symbol === 'loop1_state') {
            setLoopLight(icon, 'loop1_state', value);
        } else if (symbol === 'loop2_state') {
            setLoopLight(icon, 'loop2_state', value);
        } else if (symbol === 'loop3_state') {
            setLoopLight(icon, 'loop3_state', value);
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
        if (!icon.find('.alo-stepbar .alo-step').length) {
            rebuildStepbar(icon, data.steps);
            setActiveStep(icon, (data.bar_step >= 0 && data.bar_step < data.steps) ? data.bar_step : -1);
        }
        icon.data('alo', data);
    } else if (event.type === 'change') {
        handle(event.symbol, event.value);
    }
}
