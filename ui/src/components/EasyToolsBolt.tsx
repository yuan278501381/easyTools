import type { FC, SVGAttributes } from 'react';

export interface LogoGlyphProps extends SVGAttributes<SVGSVGElement> {
  size?: number;
  className?: string;
  fill?: string;
}

/**
 * LogoGlyph — EasyTools 高精度矢量微晶单色母版图标 (Optical Sizing Glyph)
 *
 * 1. 彻底根除 398KB Base64 内联位图的内存常驻与解码损耗 (代码量仅 ~1.5KB，降幅 99.6%)
 * 2. 原生支持 fill="currentColor" 或 fill="var(--primary)"，与系统主题及动态主题色自适应
 * 3. 采用分级折纸多重透明度光影映射，在单色/主色填充下依然完整还原 3D 丝带流转质感
 */
export const LogoGlyph: FC<LogoGlyphProps> = ({
  size = 24,
  className = '',
  fill = 'currentColor',
  style,
  ...props
}) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="100 53 1110 1110"
    width={size}
    height={size}
    className={className}
    style={{ display: 'inline-block', flexShrink: 0, verticalAlign: 'middle', ...style }}
    fill="none"
    aria-hidden="true"
    {...props}
  >
    {/* 9 组折纸切面按深度层级自底向上渲染，fillOpacity 保持多维明暗光影层次 */}
    {/* 1. 顶部向右水平回折面 (Top Return) */}
    <path
      d="M 552,333 C 601,184 658,87 790,87 L 1175,86 C 1160,178 1108,263 1030,302 C 997,319 966,328 930,328 Z"
      fill={fill}
      fillOpacity={0.98}
    />
    {/* 2. 顶部主弯曲受光面 (Top Face) */}
    <path
      d="M 229,598 C 234,573 240,540 247,511 C 269,423 289,356 319,281 C 363,165 449,98 575,89 C 636,85 710,87 790,87 C 658,87 601,184 552,333 L 544,360 L 521,450 C 502,455 489,468 484,483 L 241,614 C 228,632 216,656 207,679 C 216,656 228,632 229,598 Z"
      fill={fill}
      fillOpacity={0.78}
    />
    {/* 3. 上部折叠阴影面 (Upper Fold) */}
    <path
      d="M 247,511 C 297,413.51 415,342.14 552,333 L 544,360 C 415,364.51 303,423.55 247,511 Z"
      fill={fill}
      fillOpacity={0.46}
    />
    {/* 4. 上部折叠内侧面 (Upper Under) */}
    <path
      d="M 247,511 C 303,423.55 415,364.51 544,360 L 521,450 C 394,463.74 292,499.63 229,598 C 234,573 240,540 247,511 Z"
      fill={fill}
      fillOpacity={0.66}
    />
    {/* 5. 中部转折折叠阴影面 (Middle Fold) */}
    <path
      d="M 229,598 C 292,499.63 394,463.74 521,450 C 502,455 489,468 484,483 C 372,502.64 291,546.71 241,614 Z"
      fill={fill}
      fillOpacity={0.40}
    />
    {/* 6. 中部主受光正面 (Middle Face) */}
    <path
      d="M 136,917 C 156,847 178,751 207,679 C 216,656 228,632 241,614 C 291,546.71 372,502.64 484,483 C 593,474 781,471 1030,471 C 1099,471 1146,458 1175,420 L 1175,472 C 1175,565 1118,653 1032,693 C 998,709 962,713 928,713 L 493,713 C 337,717.42 194,780.03 136,917 Z"
      fill={fill}
      fillOpacity={0.88}
    />
    {/* 7. 下部折叠阴影面 (Bottom Fold) */}
    <path
      d="M 136,917 C 194,780.03 337,717.42 493,713 C 478,715 467,724 462,739 C 308,748.01 187,815.65 136,917 Z"
      fill={fill}
      fillOpacity={0.46}
    />
    {/* 8. 底部左转角弧面 (Bottom Left) */}
    <path
      d="M 136,917 C 187,815.65 308,748.01 462,739 C 421,851 383,982 426,1052 C 458,1106 516,1128 606,1128 L 280,1131 C 205,1087 151,1015 136,917 Z"
      fill={fill}
      fillOpacity={0.72}
    />
    {/* 9. 底部向右水平回折面 (Bottom Return) */}
    <path
      d="M 462,739 C 448,781 438,815 433,835 C 427,859 448,872 485,871 L 1119,866 C 1091,977 1012,1082 904,1115 C 867,1126 829,1128 790,1128 L 606,1128 C 516,1128 458,1106 426,1052 C 383,982 421,851 462,739 Z"
      fill={fill}
      fillOpacity={0.96}
    />
  </svg>
);

/** 兼容历史命名的别名导出 */
export const EasyToolsBolt: FC<LogoGlyphProps> = LogoGlyph;
