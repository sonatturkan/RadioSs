(() => {
    'use strict';

    const workspace = document.getElementById('live-task-view');
    const nav = workspace?.querySelector('#broadcast-right-nav');
    if (!nav) return;

    const tabs = Array.from(nav.querySelectorAll('[data-broadcast-pane]')).filter((tab) => {
        const panel = document.getElementById(tab.getAttribute('aria-controls'));
        return panel && workspace.contains(panel);
    });
    if (!tabs.length) return;

    let selected = tabs.find((tab) => tab.getAttribute('aria-selected') === 'true') || tabs[0];

    const activate = (tab, focus = false) => {
        if (!tabs.includes(tab)) return;
        selected = tab;
        for (const control of tabs) {
            const active = control === selected;
            control.setAttribute('aria-selected', String(active));
            control.tabIndex = active ? 0 : -1;
            document.getElementById(control.getAttribute('aria-controls')).hidden = !active;
        }
        if (focus) selected.focus({ preventScroll: true });
    };

    nav.addEventListener('click', (event) => {
        if (!(event.target instanceof Element)) return;
        activate(event.target.closest('[data-broadcast-pane]'));
    });

    nav.addEventListener('keydown', (event) => {
        if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
        if (!(event.target instanceof Element)) return;
        const current = tabs.indexOf(event.target.closest('[data-broadcast-pane]'));
        if (current < 0) return;
        event.preventDefault();
        const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1
            : (current + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length;
        activate(tabs[next], true);
    });

    let dragTimer = null;
    let dragTab = null;
    const cancelDragTab = () => {
        clearTimeout(dragTimer);
        dragTimer = null;
        dragTab = null;
    };
    nav.addEventListener('dragover', (event) => {
        if (!Array.from(event.dataTransfer?.types || []).includes('application/x-radios-programme-items')) return;
        const tab = event.target instanceof Element ? event.target.closest('[data-broadcast-pane]') : null;
        if (!tabs.includes(tab)) return;
        event.preventDefault();
        event.dataTransfer.dropEffect = 'copy';
        if (tab === selected) { cancelDragTab(); return; }
        if (tab === dragTab) return;
        cancelDragTab();
        dragTab = tab;
        dragTimer = setTimeout(() => { activate(tab); cancelDragTab(); }, 450);
    });
    nav.addEventListener('dragleave', (event) => {
        if (event.relatedTarget instanceof Node && nav.contains(event.relatedTarget)) return;
        cancelDragTab();
    });
    nav.addEventListener('drop', cancelDragTab);
    document.addEventListener('dragend', cancelDragTab);

    document.addEventListener('radios:profile-change', (event) => {
        cancelDragTab();
        if (event.detail?.profile === 'broadcast') activate(selected);
    });

    activate(selected);
})();
