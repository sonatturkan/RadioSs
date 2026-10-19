(() => {
    const mixerGroups = new WeakMap();
    const effectGroups = new WeakMap();
    let refreshing = false;
    const refresh = () => {
        if (refreshing) return;
        refreshing = true;
        try {
            const channels = window.RadioSDJMixer?.snapshot?.()?.channels || [];
            const mixerSignature = JSON.stringify(channels.map(({ id, label }) => [id, label]));
            document.querySelectorAll('[data-fx-source], [data-monitor-gui-source]').forEach((select) => {
                if (mixerGroups.get(select) === mixerSignature && !select.dataset.fxDesiredSource) return;
                const previous = select.dataset.fxDesiredSource || select.value;
                select.querySelector('[data-dj-mixer-options]')?.remove();
                if (channels.length) {
                    const group = document.createElement('optgroup');
                    group.label = 'DJ mixer kanalları'; group.dataset.djMixerOptions = '';
                    channels.forEach((channel) => {
                        const option = document.createElement('option'); option.value = `mixer:${channel.id}`; option.textContent = channel.label; group.append(option);
                    });
                    select.append(group);
                }
                const available = Array.from(select.options).some((option) => option.value === previous);
                select.value = available ? previous : '';
                const hadPending = Boolean(select.dataset.fxDesiredSource);
                delete select.dataset.fxDesiredSource;
                mixerGroups.set(select, mixerSignature);
                if (!available && previous || hadPending) select.dispatchEvent(new Event('change', { bubbles: true }));
            });
            const effectState = window.RadioSDJEffects?.snapshot?.()?.effects;
            const effects = Array.isArray(effectState) ? effectState.map((effect, index) => ({ id: effect.id, number: index + 1, name: effect.name || '' }))
                : Array.from(document.querySelectorAll('[data-dj-effects-rack] [data-fx-control]')).map((node, index) => ({ id: `fx-${node.dataset.fxSlot}`, number: index + 1, name: '' }));
            const effectSignature = JSON.stringify(effects);
            document.querySelectorAll('[data-monitor-gui-source]').forEach((select) => {
                if (effectGroups.get(select) === effectSignature) return;
                const previous = select.value;
                select.querySelectorAll('option').forEach((option) => { if (/^fx-\d+-(send|return|mix)$/.test(option.value)) option.remove(); });
                select.querySelectorAll('optgroup').forEach((group) => { if (!group.children.length) group.remove(); });
                if (effects.length) {
                    const group = document.createElement('optgroup'); group.label = 'Mixer efektleri'; group.dataset.djFxOptions = '';
                    effects.forEach(({ id, number, name }) => ['send', 'return', 'mix'].forEach((path) => {
                        const option = document.createElement('option'); option.value = `${id}-${path}`; option.textContent = `FX ${number}${name ? ` · ${name}` : ''} · ${path.toUpperCase()}`; group.append(option);
                    }));
                    select.append(group);
                }
                select.value = Array.from(select.options).some((option) => option.value === previous) ? previous : '';
                effectGroups.set(select, effectSignature);
                if (select.value !== previous) select.dispatchEvent(new Event('change', { bubbles: true }));
            });
            document.dispatchEvent(new CustomEvent('radios:monitor-options-change'));
        } finally { refreshing = false; }
    };
    document.addEventListener('radios:dj-mixer-channels-change', refresh);
    document.addEventListener('radios:dj-effects-change', refresh);
    refresh();
})();
