#include "components/parser.hpp"

#include <cassert>

#include "components/types.hpp"
#include "events/signal.hpp"
#include "events/signal_emitter.hpp"
#include "settings.hpp"
#include "utils/color.hpp"
#include "utils/factory.hpp"
#include "utils/memory.hpp"
#include "utils/string.hpp"

POLYBAR_NS

using namespace signals::parser;

/**
 * Create instance
 */
parser::make_type parser::make() {
  return factory_util::unique<parser>(signal_emitter::make());
}

/**
 * Construct parser instance
 */
parser::parser(signal_emitter& emitter) : m_sig(emitter) {}

/**
 * Process input string
 */
void parser::parse(const bar_settings& bar, string data) {
  while (!data.empty()) {
    size_t pos{string::npos};

    if (data.compare(0, 2, "%{") == 0 && (pos = data.find('}')) != string::npos) {
      codeblock(data.substr(2, pos - 2), bar);
      data.erase(0, pos + 1);
    } else if ((pos = data.find("%{")) != string::npos) {
      data.erase(0, text(data.substr(0, pos)));
    } else {
      data.erase(0, text(data.substr(0)));
    }
  }

  if (!m_actions.empty()) {
    throw unclosed_actionblocks(to_string(m_actions.size()) + " unclosed action block(s)");
  }
}

/**
 * Process contents within tag blocks, i.e: %{...}
 */
void parser::codeblock(string&& data, const bar_settings& bar) {
  size_t pos;

  while (data.length()) {
    data = string_util::ltrim(move(data), ' ');

    if (data.empty()) {
      break;
    }

    char tag{data[0]};

    /*
     * Contains the string from the current position to the next space or
     * closing curly bracket (})
     *
     * This may be unsuitable for some tags (e.g. action tag) to use
     * These MUST set value to the actual string they parsed from the beginning
     * of data (or a string with the same length). The length of value is used
     * to progress the cursor further.
     *
     * example:
     *
     * data = A1:echo "test": ...}
     *
     * erase(0,1)
     * -> data = 1:echo "test": ...}
     *
     * case 'A', parse_action_cmd
     * -> value = echo "test"
     *
     * Padding value
     * -> value = echo "test"0::
     *
     * erase(0, value.length())
     * -> data =  ...}
     *
     */
    string value;

    // Remove the tag
    data.erase(0, 1);

    if ((pos = data.find_first_of(" }")) != string::npos) {
      value = data.substr(0, pos);
    } else {
      value = data;
    }

    switch (tag) {
      case 'B':
        m_sig.emit(change_background{parse_color(value, bar.background)});
        break;

      case 'F':
        m_sig.emit(change_foreground{parse_color(value, bar.foreground)});
        break;

      case 'T':
        m_sig.emit(change_font{parse_fontindex(value)});
        break;

      case 'U':
        m_sig.emit(change_underline{parse_color(value, bar.underline.color)});
        m_sig.emit(change_overline{parse_color(value, bar.overline.color)});
        break;

      case 'u':
        m_sig.emit(change_underline{parse_color(value, bar.underline.color)});
        break;

      case 'o':
        m_sig.emit(change_overline{parse_color(value, bar.overline.color)});
        break;

      case 'R':
        m_sig.emit(reverse_colors{});
        break;

      case 'O':
        m_sig.emit(offset_pixel{static_cast<int>(std::strtol(value.c_str(), nullptr, 10))});
        break;

      case 'l':
        m_sig.emit(change_alignment{alignment::LEFT});
        break;

      case 'c':
        m_sig.emit(change_alignment{alignment::CENTER});
        break;

      case 'r':
        m_sig.emit(change_alignment{alignment::RIGHT});
        break;

      case '+':
        m_sig.emit(attribute_set{parse_attr(value[0])});
        break;

      case '-':
        m_sig.emit(attribute_unset{parse_attr(value[0])});
        break;

      case '!':
        m_sig.emit(attribute_toggle{parse_attr(value[0])});
        break;

      case 'A': {
        bool has_btn_id = (data[0] != ':');
        if (isdigit(data[0]) || !has_btn_id) {
          value = parse_action_cmd(data.substr(has_btn_id ? 1 : 0));
          mousebtn btn = parse_action_btn(data);
          m_actions.push_back(static_cast<int>(btn));

          // Unescape colons inside command before sending it to the renderer
          auto cmd = string_util::replace_all(value, "\\:", ":");
          m_sig.emit(action_begin{action{btn, cmd}});

          /*
           * make sure value has the same length as the inside of the action
           * tag which is btn_id + ':' + value + ':'
           */
          if (has_btn_id) {
            value += "0";
          }
          value += "::";
        } else if (!m_actions.empty()) {
          m_sig.emit(action_end{parse_action_btn(value)});
          m_actions.pop_back();
        }
        break;
      }

      // Internal Polybar control tags
      case 'P':
        m_sig.emit(control{parse_control(value)});
        break;

      default:
        throw unrecognized_token("Unrecognized token '" + string{tag} + "'");
    }

    if (!data.empty()) {
      // Remove the parsed string from data
      data.erase(0, !value.empty() ? value.length() : 1);
    }
  }
}

/**
 * Process text contents
 */
size_t parser::text(string&& data) {
#ifdef DEBUG_WHITESPACE
  string::size_type p;
  while ((p = data.find(' ')) != string::npos) {
    data.replace(p, 1, "-"s);
  }
#endif

  m_sig.emit(signals::parser::text{forward<string>(data)});
  return data.size();
}

/**
 * Process color hex string and convert it to the correct value
 */
rgba parser::parse_color(const string& s, rgba fallback) {
  if (!s.empty() && s[0] != '-') {
    rgba ret = rgba{s};

    if (!ret.has_color() || ret.type() == rgba::ALPHA_ONLY) {
      logger::make().warn(
          "Invalid color in formatting tag detected: \"%s\", using fallback \"%s\". This is an issue with one of your "
          "formatting tags. If it is not, please report this as a bug.",
          s, static_cast<string>(fallback));
      return fallback;
    }

    return ret;
  }
  return fallback;
}

/**
 * Process font index and convert it to the correct value
 */
int parser::parse_fontindex(const string& s) {
  if (s.empty() || s[0] == '-') {
    return 0;
  }

  try {
    return std::stoul(s, nullptr, 10);
  } catch (const std::invalid_argument& err) {
    return 0;
  }
}

/**
 * Process attribute token and convert it to the correct value
 */
attribute parser::parse_attr(const char attr) {
  switch (attr) {
    case 'o':
      return attribute::OVERLINE;
    case 'u':
      return attribute::UNDERLINE;
    default:
      throw unrecognized_token("Unrecognized attribute '" + string{attr} + "'");
  }
}

/**
 * Process action button token and convert it to the correct value
 */
mousebtn parser::parse_action_btn(const string& data) {
  if (data[0] == ':') {
    return mousebtn::LEFT;
  } else if (isdigit(data[0])) {
    return static_cast<mousebtn>(data[0] - '0');
  } else if (!m_actions.empty()) {
    return static_cast<mousebtn>(m_actions.back());
  } else {
    return mousebtn::NONE;
  }
}

/**
 * Process action command string
 *
 * data is the action cmd surrounded by unescaped colons followed by an
 * arbitrary string
 *
 * Returns everything inside the unescaped colons as is
 */
string parser::parse_action_cmd(string&& data) {
  if (data[0] != ':') {
    return "";
  }

  size_t end{1};
  while ((end = data.find(':', end)) != string::npos && data[end - 1] == '\\') {
    end++;
  }

  if (end == string::npos) {
    return "";
  }

  return data.substr(1, end - 1);
}

controltag parser::parse_control(const string& data) {
  if (data.length() != 1) {
    return controltag::NONE;
  }

  switch (data[0]) {
    case 'R':
      return controltag::R;
      break;
    default:
      return controltag::NONE;
      break;
  }
}

POLYBAR_NS_END
