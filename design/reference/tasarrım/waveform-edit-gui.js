(() => {
    'use strict';

    const toolNames = {
        select: 'Seçim',
        'cue-in': 'Cue başlangıcı',
        'cue-out': 'Cue bitişi',
        split: 'Bölme',
        fade: 'Fade'
    };
    const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
    const initialState = () => ({ position: 50, in: 5, out: 95, cut: null, fade: false, zoom: 1 });

    document.querySelectorAll('[data-waveform-editor-gui]').forEach(editor => {
        const surface = editor.querySelector('[data-waveform-gui-surface]');
        if (!surface) return;
        const workspace = editor.closest('.waveform-panel') || editor.parentElement;
        const transport = workspace?.querySelector('[data-waveform-gui-transport]');
        const status = editor.querySelector('[data-waveform-gui-status]');
        const tools = Array.from(editor.querySelectorAll('[data-waveform-tool]'));
        const actions = Array.from(editor.querySelectorAll('[data-waveform-action]'));
        const undo = [];
        const redo = [];
        let state = initialState();
        let tool = 'select';
        let playing = false;
        let drag = null;

        const announce = message => {
            if (status) status.textContent = `GUI taslağı · ${message}`;
        };
        const sameState = (a, b) => Object.keys(a).every(key => a[key] === b[key]);
        const remember = previous => {
            if (sameState(previous, state)) return false;
            undo.push({ ...previous });
            if (undo.length > 40) undo.shift();
            redo.length = 0;
            return true;
        };
        const renderTransport = () => {
            editor.dataset.playing = String(playing);
            if (!transport) return;
            const label = playing ? 'Duraklat' : 'Oynat';
            transport.setAttribute('aria-pressed', String(playing));
            transport.setAttribute('aria-label', `${label} · GUI taslağı`);
            transport.title = `${label} · GUI taslağı`;
            transport.querySelector('[data-waveform-play-icon]')?.toggleAttribute('hidden', playing);
            transport.querySelector('[data-waveform-pause-icon]')?.toggleAttribute('hidden', !playing);
            const text = transport.querySelector('[data-waveform-transport-label]');
            if (text) text.textContent = label;
        };
        const render = () => {
            editor.style.setProperty('--waveform-gui-position', `${state.position}%`);
            editor.style.setProperty('--waveform-gui-in', `${state.in}%`);
            editor.style.setProperty('--waveform-gui-out', `${state.out}%`);
            editor.style.setProperty('--waveform-gui-cut', `${state.cut ?? 50}%`);
            editor.style.setProperty('--waveform-gui-zoom', String(state.zoom));
            editor.dataset.cut = String(state.cut !== null);
            editor.dataset.fade = String(state.fade);
            editor.dataset.tool = tool;
            tools.forEach(button => button.setAttribute('aria-pressed', String(button.dataset.waveformTool === tool)));
            actions.forEach(button => {
                const action = button.dataset.waveformAction;
                button.disabled = (action === 'undo' && !undo.length)
                    || (action === 'redo' && !redo.length)
                    || (action === 'zoom-in' && state.zoom >= 4)
                    || (action === 'zoom-out' && state.zoom <= 1);
            });
            surface.setAttribute('aria-label', `Dalga biçimi GUI taslağı; ${toolNames[tool]}, konum yüzde ${Math.round(state.position)}, yakınlaştırma ${state.zoom} kat`);
            renderTransport();
        };
        const editAt = value => {
            const position = Math.round(clamp(value, 0, 100) * 10) / 10;
            if (tool === 'cue-in') state.in = Math.min(position, state.out - 1);
            else if (tool === 'cue-out') state.out = Math.max(position, state.in + 1);
            else if (tool === 'split') state.cut = position;
            else if (tool === 'fade') {
                state.fade = true;
                state.position = position;
            } else state.position = position;
            render();
        };
        const toolPosition = () => tool === 'cue-in' ? state.in : tool === 'cue-out' ? state.out : tool === 'split' ? (state.cut ?? state.position) : state.position;
        const describeEdit = () => `${toolNames[tool]} · %${Math.round(toolPosition())}`;
        const pointerPosition = event => {
            const bounds = surface.getBoundingClientRect();
            return bounds.width ? (event.clientX - bounds.left) / bounds.width * 100 : state.position;
        };

        tools.forEach(button => button.addEventListener('click', () => {
            const selected = button.dataset.waveformTool;
            if (!Object.hasOwn(toolNames, selected)) return;
            tool = selected;
            render();
            announce(`${toolNames[tool]} aracı seçildi`);
        }));
        actions.forEach(button => button.addEventListener('click', () => {
            const action = button.dataset.waveformAction;
            if (action === 'undo' || action === 'redo') {
                const from = action === 'undo' ? undo : redo;
                const to = action === 'undo' ? redo : undo;
                if (!from.length) return;
                to.push({ ...state });
                state = from.pop();
                announce(action === 'undo' ? 'Düzenleme geri alındı' : 'Düzenleme yinelendi');
            } else {
                const previous = { ...state };
                if (action === 'zoom-in') state.zoom = clamp(state.zoom + 1, 1, 4);
                else if (action === 'zoom-out') state.zoom = clamp(state.zoom - 1, 1, 4);
                else if (action === 'fit') state.zoom = 1;
                else return;
                remember(previous);
                announce(action === 'fit' ? 'Görünüm sığdırıldı' : `Yakınlaştırma ${state.zoom} kat`);
            }
            render();
        }));

        surface.addEventListener('pointerdown', event => {
            if (event.button !== 0 || drag) return;
            event.preventDefault();
            surface.focus({ preventScroll: true });
            drag = { id: event.pointerId, previous: { ...state } };
            surface.setPointerCapture(event.pointerId);
            editAt(pointerPosition(event));
        });
        surface.addEventListener('pointermove', event => {
            if (drag?.id === event.pointerId) editAt(pointerPosition(event));
        });
        const finishDrag = event => {
            if (drag?.id !== event.pointerId) return;
            remember(drag.previous);
            drag = null;
            if (surface.hasPointerCapture(event.pointerId)) surface.releasePointerCapture(event.pointerId);
            render();
            announce(describeEdit());
        };
        surface.addEventListener('pointerup', finishDrag);
        surface.addEventListener('pointercancel', event => {
            if (drag?.id !== event.pointerId) return;
            state = drag.previous;
            drag = null;
            if (surface.hasPointerCapture(event.pointerId)) surface.releasePointerCapture(event.pointerId);
            render();
            announce('Sürükleme iptal edildi');
        });
        surface.addEventListener('lostpointercapture', event => {
            if (drag?.id === event.pointerId) finishDrag(event);
        });
        surface.addEventListener('keydown', event => {
            if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
            event.preventDefault();
            const previous = { ...state };
            const step = event.shiftKey ? 5 : 1;
            const current = toolPosition();
            const position = event.key === 'Home' ? 0 : event.key === 'End' ? 100 : current + (event.key === 'ArrowLeft' ? -step : step);
            editAt(position);
            remember(previous);
            render();
            announce(describeEdit());
        });
        transport?.addEventListener('click', () => {
            playing = !playing;
            renderTransport();
            announce(playing ? 'Oynat seçildi · görsel taslak' : 'Duraklat seçildi · görsel taslak');
        });
        document.addEventListener('radios:programme-change', () => {
            playing = false;
            renderTransport();
        });
        document.addEventListener('radios:profile-change', () => {
            playing = false;
            renderTransport();
        });
        render();
        announce('Düzenleme aracı seç; işaretleri dalga biçimi üzerinde taşı');
    });
})();
