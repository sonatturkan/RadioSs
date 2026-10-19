(() => {
    const search = document.querySelector('[data-library-search]');
    if (!search) return;

    const query = search.elements.namedItem('query');
    const type = search.elements.namedItem('type');
    const status = document.querySelector('[data-library-status]');
    const detail = document.querySelector('[data-library-detail]');

    // A missing library is not an empty search result. Keep that distinction
    // visible until actual library data is available.
    const showPendingLibrary = () => {
        const term = query.value.trim();
        const selectedType = type.options[type.selectedIndex].text;
        status.textContent = 'Kütüphane verisi bekleniyor';
        detail.textContent = term
            ? `“${term}” için arama, içerikler geldiğinde yapılabilir.`
            : type.value === 'all'
                ? 'İçerikler geldiğinde arama sonuçları burada görünecek.'
                : `${selectedType} içerikleri geldiğinde burada görünecek.`;
    };

    search.addEventListener('submit', (event) => {
        event.preventDefault();
        showPendingLibrary();
    });
    type.addEventListener('change', showPendingLibrary);
    query.addEventListener('input', () => {
        if (!query.value.trim()) showPendingLibrary();
    });
})();
