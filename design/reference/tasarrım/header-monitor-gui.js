(() => {
    document.querySelectorAll('[data-header-monitor-gui]').forEach((module) => {
        const source = module.querySelector('[data-monitor-gui-source]');
        const destination = module.querySelector('[data-monitor-gui-output]');
        const cue = module.querySelector('[data-monitor-gui-cue]');
        const status = module.querySelector('[data-monitor-gui-status]');
        if (!source || !cue || !status) return;
        const isOutput = module.dataset.monitorGuiKind === 'output';
        const action = isOutput ? 'MON' : 'CUE';

        // Each control previews its own GUI selection only; it does not route audio.
        function render() {
            const selected = Boolean(source.value) && (!destination || Boolean(destination.value));
            const name = source.selectedOptions[0]?.textContent.trim() || '';
            const outputName = destination?.selectedOptions[0]?.textContent.trim() || '';
            const route = destination ? `${name} → ${outputName}` : name;
            const active = selected && cue.getAttribute('aria-pressed') === 'true';
            cue.disabled = !selected;
            cue.setAttribute('aria-pressed', String(active));
            cue.title = selected
                ? `${route} · ${isOutput ? 'Monitor çıkışı' : 'Kulaklık CUE'} seçimi`
                : '';
            status.textContent = selected
                ? `${route} · ${action} ${active ? 'seçili' : 'kapalı'}`
                : '—';
        }

        const clearSelection = () => {
            cue.setAttribute('aria-pressed', 'false');
            render();
        };
        source.addEventListener('change', clearSelection);
        destination?.addEventListener('change', clearSelection);
        cue.addEventListener('click', () => {
            if (cue.disabled) return;
            cue.setAttribute('aria-pressed', String(cue.getAttribute('aria-pressed') !== 'true'));
            render();
        });
        document.addEventListener('radios:monitor-options-change', render);
        render();
    });
})();
