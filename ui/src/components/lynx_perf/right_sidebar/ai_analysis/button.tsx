// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import React from 'react';

interface ButtonProps {
  children: React.ReactNode;
  onClick?: () => void;
  className?: string;
  style?: React.CSSProperties;
  disabled?: boolean;
  onMouseEnter?: (e: React.MouseEvent<HTMLButtonElement>) => void;
  onMouseLeave?: (e: React.MouseEvent<HTMLButtonElement>) => void;
}

export class Button extends React.Component<ButtonProps> {
  static defaultProps = {
    disabled: false
  };

  render() {
    const { children, onClick, style, disabled, onMouseEnter, onMouseLeave } = this.props;

    return (
      <button
        onClick={onClick}
        disabled={disabled}
        style={{
          display: 'inline-flex',
          alignItems: 'center',
          justifyContent: 'center',
          padding: '8px 16px',
          fontSize: '0.875rem',
          fontWeight: 500,
          borderRadius: '12px',
          border: '1px solid transparent',
          cursor: disabled ? 'not-allowed' : 'pointer',
          transition: 'all 0.3s ease-in-out',
          textDecoration: 'none',
          background: 'none',
          outline: 'none',
          ...style
        }}
        onMouseEnter={onMouseEnter}
        onMouseLeave={onMouseLeave}
        onFocus={(e) => {
          e.currentTarget.style.boxShadow = '0 0 0 2px rgba(59, 130, 246, 0.5), 0 0 0 4px rgba(59, 130, 246, 0.1)';
        }}
        onBlur={(e) => {
          e.currentTarget.style.boxShadow = 'none';
        }}
      >
        {children}
      </button>
    );
  }
}