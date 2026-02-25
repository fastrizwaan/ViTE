import xml.etree.ElementTree as ET
import json
import os
import re
import sys

def parse_rgba(val):
    m = re.match(r'#rgba\((\d+),(\d+),(\d+),([\d.]+)\)', val)
    if m:
        r, g, b, a = int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))
        alpha = int(a * 255)
        return f"#{r:02x}{g:02x}{b:02x}{alpha:02x}"
    return val

def convert_theme(xml_path, out_dir):
    try:
        tree = ET.parse(xml_path)
    except Exception as e:
        print(f"Skipping {xml_path}: {e}")
        return

    root = tree.getroot()
    if root.tag != 'style-scheme':
        print(f"Skipping {xml_path} - not a style-scheme")
        return
        
    theme_name = root.get('name')
    if not theme_name:
        theme_name = os.path.basename(xml_path).replace('.xml', '')
        
    is_dark = True
    metadata = root.find('metadata')
    if metadata is not None:
        for prop in metadata.findall('property'):
            if prop.get('name') == 'variant':
                if prop.text == 'light':
                    is_dark = False
    
    colors = {}
    for c in root.findall('color'):
        name = c.get('name')
        val = c.get('value')
        if val and val.startswith('#rgba'):
            val = parse_rgba(val)
        colors[name] = val
        
    def resolve_color(val):
        if not val:
            return None
        # Could be a direct hex
        if val.startswith('#'):
            if val.startswith('#rgba'):
                return parse_rgba(val)
            return val
        # Or a named color
        return colors.get(val, val)

    vscode_colors = {}
    token_colors = []
    
    def add_token(scope_list, fg=None, bg=None, bold=False, italic=False, underline=False):
        if not fg and not bg and not bold and not italic and not underline:
            return
        settings = {}
        if fg: settings['foreground'] = fg
        if bg: settings['background'] = bg
        
        styles = []
        if bold: styles.append('bold')
        if italic: styles.append('italic')
        if underline: styles.append('underline')
        if styles: settings['fontStyle'] = ' '.join(styles)
            
        token_colors.append({
            "scope": scope_list,
            "settings": settings
        })

    for s in root.findall('style'):
        name = s.get('name')
        if not name: continue
        
        fg = resolve_color(s.get('foreground'))
        bg = resolve_color(s.get('background'))
        bold = s.get('bold') == 'true'
        italic = s.get('italic') == 'true'
        underline = s.get('italic') == 'true' # fallback approximation if requested
        if s.get('underline') == 'true' or s.get('underline') == 'single':
            underline = True
        
        # 1. Map to Global VSCode Editor Elements
        if name == 'text':
            if bg: vscode_colors['editor.background'] = bg
            if fg: vscode_colors['editor.foreground'] = fg
        elif name == 'selection':
            if bg: vscode_colors['editor.selectionBackground'] = bg
            if fg: vscode_colors['editor.selectionForeground'] = fg
        elif name == 'current-line':
            if bg: vscode_colors['editor.lineHighlightBackground'] = bg
        elif name == 'line-numbers':
            if fg: vscode_colors['editorLineNumber.foreground'] = fg
            # if bg: vscode_colors['editorLineNumber.background'] = bg
        elif name == 'cursor':
            if fg: vscode_colors['editorCursor.foreground'] = fg
        elif name == 'search-match':
            if bg: vscode_colors['editor.findMatchBackground'] = bg
        elif name == 'bracket-match':
            if bg: vscode_colors['editorBracketMatch.background'] = bg
            if fg: vscode_colors['editorBracketMatch.border'] = fg
            
        lang_suffix = ""
        base_name = name
        if ':' in name:
            parts = name.split(':', 1)
            # Map some gnome prefixes to standard vscode suffixes
            # e.g. "c:comment" -> "comment.c"
            lang_prefix = parts[0]
            if lang_prefix != 'def':
                lang_suffix = "." + lang_prefix
            base_name = parts[1]

        # 2. Syntax Token Mapping (using standard TS scopes mostly)
        scope_targets = []
        
        # General definition mappings
        if 'comment' in base_name: scope_targets = ['comment']
        elif 'string' in base_name or 'template-literal' in base_name or 'attribute-value' in base_name or 'included-file' in base_name: scope_targets = ['string']
        elif 'built-in-function' in base_name or 'built-in-method' in base_name: scope_targets = ['support.function']
        elif 'built-in-constructor' in base_name: scope_targets = ['support.type']
        elif 'function' in base_name or 'method' in base_name or 'command' in base_name: scope_targets = ['entity.name.function']
        elif 'keyword' in base_name: scope_targets = ['keyword']
        elif 'statement' in base_name: scope_targets = ['keyword.control']
        elif 'type' in base_name or 'class' in base_name: scope_targets = ['entity.name.type']
        elif 'boolean' in base_name: scope_targets = ['constant.language.boolean']
        elif 'null' in base_name: scope_targets = ['constant.language.null']
        elif 'constant' in base_name: scope_targets = ['constant']
        elif 'floating-point' in base_name or 'decimal' in base_name or 'number' in base_name: scope_targets = ['constant.numeric']
        elif 'parameter' in base_name: scope_targets = ['variable.parameter']
        elif 'property' in base_name: scope_targets = ['variable.other.property']
        elif 'identifier' in base_name or 'variable' in base_name: scope_targets = ['variable']
        elif 'preprocessor' in base_name or 'macro' in base_name or 'include' in base_name: scope_targets = ['keyword.control.directive']
        elif 'special-char' in base_name: scope_targets = ['constant.character.escape']
        elif 'operator' in base_name: scope_targets = ['keyword.operator']
        elif 'punctuation' in base_name: scope_targets = ['punctuation']
        elif 'storage-class' in base_name: scope_targets = ['storage.modifier']
        elif 'enum-name' in base_name: scope_targets = ['entity.name.type.enum']
        elif 'attribute-name' in base_name: scope_targets = ['entity.other.attribute-name']
        elif 'tag' in base_name: scope_targets = ['entity.name.tag']
        
        if scope_targets:
            # Append language suffix to make it textmate compatible
            final_scopes = [s + lang_suffix for s in scope_targets]
            add_token(final_scopes, fg, bg, bold, italic, underline)
            
    # Generic backup scopes if any are completely missing in the GNOME theme (sometimes they rely heavily on defaults)
    
    out_dict = {
        "name": theme_name,
        "type": "dark" if is_dark else "light",
        "colors": vscode_colors,
        "tokenColors": token_colors
    }
    
    out_name = os.path.basename(xml_path).replace('.xml', '.json')
    out_path = os.path.join(out_dir, out_name)
    with open(out_path, 'w') as f:
        json.dump(out_dict, f, indent=4)
        print(f"[{theme_name}] Converted {os.path.basename(xml_path)} -> {out_name}")

def main():
    in_dir = '/var/home/rizvan/ViTE/gnome-text-editor-49.0/src/styles'
    out_dir = '/var/home/rizvan/ViTE/vscode-themes'
    os.makedirs(out_dir, exist_ok=True)
    
    if not os.path.isdir(in_dir):
        print(f"Error: Could not find source directory {in_dir}")
        sys.exit(1)
        
    count = 0
    for f in os.listdir(in_dir):
        if f.endswith('.xml'):
            convert_theme(os.path.join(in_dir, f), out_dir)
            count += 1
            
    print(f"\nSuccessfully created {count} JSON themes in {out_dir}")

if __name__ == '__main__':
    main()
