#!/usr/bin/env python3

import sys
import json
import re
import os

UI_MAPPING = {
    "editor.background": "editor_bg",
    "editor.foreground": "editor_fg",
    "editorGutter.background": "gutter_bg",
    "editorLineNumber.foreground": "gutter_fg",
    "editorLineNumber.activeForeground": "gutter_active_fg",
    "editor.lineHighlightBackground": "line_highlight",
    "editor.selectionBackground": "selection",
    "editorCursor.foreground": "cursor_color",
    "editor.findMatchBackground": "find_match",
    "editor.findMatchHighlightBackground": "find_match_highlight",
    
    "tab.activeBackground": "tab_active_bg",
    "tab.activeForeground": "tab_active_fg",
    "tab.inactiveBackground": "tab_inactive_bg",
    "tab.inactiveForeground": "tab_inactive_fg",
    "tab.border": "tab_border",
    
    "titleBar.activeBackground": "titlebar_bg",
    "titleBar.activeForeground": "titlebar_fg",
    "statusBar.background": "statusbar_bg",
    "statusBar.foreground": "statusbar_fg",
    
    "scrollbarSlider.background": "scrollbar_bg",
    "scrollbarSlider.hoverBackground": "scrollbar_hover",
    "scrollbarSlider.activeBackground": "scrollbar_active",
    
    "sideBar.background": "window_bg",
    "sideBar.foreground": "window_fg",
    "dropdown.background": "popover_bg",
    "dropdown.foreground": "popover_fg",
    "editorWidget.background": "dialog_bg",
    "editorWidget.foreground": "dialog_fg",
    "editorWidget.border": "dialog_titlebar_bg",
    "focusBorder": "border_color",
    "list.hoverBackground": "hover_bg",
    "descriptionForeground": "dim_fg",
    "button.background": "accent_bg",
    
    "input.background": "entry_bg",
    "input.foreground": "entry_fg",
    "input.border": "entry_border",
    "focusBorder": "entry_active_border"
}

def remove_comments(json_str):
    """Remove C-style comments (// and /* */) from a JSON string."""
    pattern = r'(\".*?\"|\'.*?\')|(/\*.*?\*/|//[^\r\n]*$)'
    regex = re.compile(pattern, re.MULTILINE | re.DOTALL)
    
    def _replacer(match):
        if match.group(2) is not None:
            return ""
        else:
            return match.group(1)
            
    return regex.sub(_replacer, json_str)

def convert_theme(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as f:
        raw_json = f.read()
        
    cleaned_json = remove_comments(raw_json)
    
    try:
        theme_data = json.loads(cleaned_json)
    except json.JSONDecodeError as e:
        print(f"Error parsing JSON: {e}")
        sys.exit(1)
        
    theme_name = theme_data.get("name", os.path.splitext(os.path.basename(input_file))[0])
    theme_type = theme_data.get("type", "dark")
    is_dark = str(theme_type.lower() == "dark").lower()
    
    colors = theme_data.get("colors", {})
    token_colors = theme_data.get("tokenColors", [])
    
    yaml_lines = []
    yaml_lines.append(f"version: 1")
    yaml_lines.append(f"name: {theme_name} (Converted)")
    yaml_lines.append(f"is_dark: {is_dark}")
    yaml_lines.append("")
    yaml_lines.append("ui:")
    
    mapped_ui = {}
    for vscode_key, vite_key in UI_MAPPING.items():
        if vscode_key in colors:
            mapped_ui[vite_key] = colors[vscode_key]
            
    # Apply standard fallbacks for missing critical UI colors
    if "gutter_bg" not in mapped_ui and "editor_bg" in mapped_ui:
        mapped_ui["gutter_bg"] = mapped_ui["editor_bg"]
    if "gutter_active_fg" not in mapped_ui and "editor_fg" in mapped_ui:
        mapped_ui["gutter_active_fg"] = mapped_ui["editor_fg"]
        
    for vite_key, color in mapped_ui.items():
        yaml_lines.append(f"  {vite_key}: \"{color}\"")
        
    yaml_lines.append("")
    yaml_lines.append("syntax:")
    yaml_lines.append("  common:")
    
    for token in token_colors:
        scope = token.get("scope")
        settings = token.get("settings", {})
        
        if not scope or not settings:
            continue
            
        fg = settings.get("foreground", "")
        font_style = settings.get("fontStyle", "")
        
        if not fg and not font_style:
            continue
            
        val_str = f"{fg} {font_style}".strip()
        
        if isinstance(scope, str):
            scopes = [s.strip() for s in scope.split(",")]
        elif isinstance(scope, list):
            scopes = scope
        else:
            continue
            
        for s in scopes:
            if s:
                yaml_lines.append(f"    {s}: \"{val_str}\"")
                
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write("\n".join(yaml_lines) + "\n")
        
    print(f"Successfully converted {input_file} to {output_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 vscode2vite-yaml-theme.py <input.json> [output.yaml]")
        sys.exit(1)
        
    input_file = sys.argv[1]
    
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        base_name = os.path.splitext(os.path.basename(input_file))[0]
        output_file = f"{base_name}.yaml"
        
    convert_theme(input_file, output_file)
