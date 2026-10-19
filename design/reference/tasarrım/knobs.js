document.querySelectorAll(".knob-input[type='range']").forEach((input) => {
    const control = input.closest(".knob-control");
    const shell = control?.querySelector(".knob-shell");
    const output = control?.querySelector(".knob-value");
    if (!shell || !output) return;
    const outputs = Array.from(document.querySelectorAll("output"))
        .filter((candidate) => candidate.htmlFor.contains(input.id));

    const min = input.min === "" ? 0 : Number(input.min);
    const max = input.max === "" ? 100 : Number(input.max);
    const step = input.step === "any" ? 0 : Number(input.step || 1);
    const center = input.dataset.center === undefined ? NaN : Number(input.dataset.center);
    const unit = input.dataset.unit ?? "dB";
    const clamp = (value) => Math.min(max, Math.max(min, value));
    let drag = null;

    function render() {
        const value = Number(input.value);
        let angle = max === min ? 0 : ((value - min) / (max - min)) * 270 - 135;
        if (min < center && center < max) {
            angle = value <= center
                ? -135 + ((value - min) / (center - min)) * 135
                : ((value - center) / (max - center)) * 135;
        }
        const number = Number(value.toFixed(4));
        const signed = input.dataset.signed !== "false";
        const text = `${signed && number > 0 ? "+" : ""}${number}${unit ? ` ${unit}` : ""}`;
        control.style.setProperty("--knob-angle", `${angle}deg`);
        outputs.forEach((candidate) => { candidate.value = text; });
        input.setAttribute("aria-valuetext", text);
    }

    function setValue(value) {
        const snapped = step > 0 ? min + Math.round((value - min) / step) * step : value;
        const next = String(Number(clamp(snapped).toFixed(10)));
        if (Number(input.value) === Number(next)) return;
        input.value = next;
        input.dispatchEvent(new Event("input", { bubbles: true }));
    }

    function finishDrag(event) {
        if (!drag || event.pointerId !== drag.pointerId) return;
        const changed = Number(input.value) !== drag.initialValue;
        drag = null;
        control.classList.remove("is-dragging");
        if (shell.hasPointerCapture(event.pointerId)) shell.releasePointerCapture(event.pointerId);
        if (changed) input.dispatchEvent(new Event("change", { bubbles: true }));
    }

    function reset(event) {
        if (input.disabled) return;
        event.preventDefault();
        const previous = Number(input.value);
        const resetValue = Number(input.dataset.resetValue ?? 0);
        setValue(clamp(Number.isFinite(resetValue) ? resetValue : 0));
        if (Number(input.value) !== previous) input.dispatchEvent(new Event("change", { bubbles: true }));
    }

    input.addEventListener("input", render);
    input.addEventListener("change", render);
    input.addEventListener("keydown", (event) => {
        if (event.key.toLowerCase() === "r" && !event.ctrlKey && !event.metaKey && !event.altKey) reset(event);
    });
    shell.style.touchAction = "none";
    shell.style.userSelect = "none";

    shell.addEventListener("pointerdown", (event) => {
        if (input.disabled || drag || event.button !== 0) return;
        event.preventDefault();
        input.focus({ preventScroll: true });
        drag = {
            pointerId: event.pointerId,
            x: event.clientX,
            y: event.clientY,
            value: Number(input.value),
            initialValue: Number(input.value)
        };
        shell.setPointerCapture(event.pointerId);
        control.classList.add("is-dragging");
    });

    shell.addEventListener("pointermove", (event) => {
        if (!drag || event.pointerId !== drag.pointerId) return;
        event.preventDefault();
        const distance = (event.clientX - drag.x) + (drag.y - event.clientY);
        drag.value = clamp(drag.value + distance * (max - min) / 180);
        drag.x = event.clientX;
        drag.y = event.clientY;
        setValue(drag.value);
    });

    shell.addEventListener("pointerup", finishDrag);
    shell.addEventListener("pointercancel", finishDrag);
    shell.addEventListener("lostpointercapture", finishDrag);
    shell.addEventListener("dblclick", reset);

    render();
});

document.querySelectorAll(".knob-input[type='range'][data-linked-knob]").forEach((linkedInput) => {
    const sourceInput = document.getElementById(linkedInput.dataset.linkedKnob);
    if (!sourceInput?.matches(".knob-input[type='range']") || sourceInput === linkedInput) return;

    let synchronizing = false;

    function synchronize(from, to, eventType) {
        if (synchronizing) return;
        synchronizing = true;
        try {
            to.value = from.value;
            to.dispatchEvent(new Event(eventType, { bubbles: true }));
        } finally {
            synchronizing = false;
        }
    }

    ["input", "change"].forEach((eventType) => {
        sourceInput.addEventListener(eventType, () => synchronize(sourceInput, linkedInput, eventType));
        linkedInput.addEventListener(eventType, () => synchronize(linkedInput, sourceInput, eventType));
    });

    // A copied control starts at the value of its existing audio control.
    synchronize(sourceInput, linkedInput, "input");
});
