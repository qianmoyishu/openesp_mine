import type { Component } from 'solid-js';

type SwitchProps = {
  checked: boolean;
  onChange?: (checked: boolean) => void;
  label?: string;
  hint?: string;
  disabled?: boolean;
  class?: string;
};

export const Switch: Component<SwitchProps> = (props) => {
  return (
    <label
      class={[
        'flex items-center gap-2 text-[0.82rem] text-[var(--color-text-secondary)] select-none',
        props.disabled ? 'opacity-50 cursor-not-allowed' : 'cursor-pointer',
        props.class ?? '',
      ]
        .filter(Boolean)
        .join(' ')}
    >
      <span
        class={[
          'relative inline-flex h-5 w-9 shrink-0 rounded-full transition border',
          props.checked
            ? 'bg-[var(--color-accent)]/80 border-[var(--color-accent-soft)]'
            : 'bg-white/5 border-[var(--color-border-subtle)]',
        ].join(' ')}
      >
        <span
          class={[
            'absolute top-0.5 h-4 w-4 rounded-full bg-white shadow transition-all',
            props.checked ? 'left-[1.125rem]' : 'left-0.5',
          ].join(' ')}
        />
        <input
          type="checkbox"
          class="sr-only"
          checked={props.checked}
          disabled={props.disabled}
          onChange={(event) => props.onChange?.(event.currentTarget.checked)}
        />
      </span>
      <span class="flex flex-col">
        {props.label && <span class="text-[var(--color-text-primary)] text-[0.82rem]">{props.label}</span>}
        {props.hint && <span class="text-[0.7rem] text-[var(--color-text-muted)]">{props.hint}</span>}
      </span>
    </label>
  );
};
