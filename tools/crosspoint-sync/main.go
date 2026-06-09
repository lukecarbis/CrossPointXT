package main

import (
	"context"
	"encoding/xml"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/charmbracelet/bubbles/list"
	"github.com/charmbracelet/bubbles/progress"
	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
)

const (
	defaultCrossPointURL = "http://crosspoint.local"
	defaultLocalDir      = "."
	defaultRemoteDir     = "/notes"
)

var (
	highlightColor   = lipgloss.Color("39")
	appStyle         = lipgloss.NewStyle().Padding(1, 2)
	titleStyle       = lipgloss.NewStyle().Bold(true).Foreground(highlightColor)
	inactiveTabStyle = lipgloss.NewStyle().
				Border(tabBorderWithBottom("┴", "─", "┴"), true).
				BorderForeground(highlightColor).
				Foreground(lipgloss.Color("245")).
				Padding(0, 1)
	activeTabStyle = inactiveTabStyle.
			Border(tabBorderWithBottom("┘", " ", "└"), true).
			Foreground(lipgloss.Color("230")).
			Bold(true)
	tabWindowStyle = lipgloss.NewStyle().
			BorderForeground(highlightColor).
			Padding(1, 1).
			Border(lipgloss.NormalBorder()).
			UnsetBorderTop()
	subtleStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	labelStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("245"))
	valueStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("252"))
	helpStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("244"))
	statusStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("220"))
	successStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("42"))
	warningStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("214"))
	errorStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("203"))
	cursorStyle  = lipgloss.NewStyle().Foreground(highlightColor).Bold(true)
	checkStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("42")).Bold(true)
)

type note struct {
	RelPath    string
	RemotePath string
	Size       int64
}

type noteItem struct {
	n           note
	selected    bool
	localExists bool
	tag         string
	tagStyle    lipgloss.Style
}

func (i noteItem) Title() string {
	check := " "
	if i.selected {
		check = "x"
	}
	size := ""
	if i.n.Size > 0 {
		size = " " + subtleStyle.Render("("+humanBytes(i.n.Size)+")")
	}
	return fmt.Sprintf("[%s] %s%s", checkStyle.Render(check), i.n.RelPath, size)
}

func (i noteItem) Description() string {
	return ""
}

func (i noteItem) FilterValue() string {
	return i.n.RelPath
}

type noteDelegate struct{}

func (d noteDelegate) Height() int {
	return 1
}

func (d noteDelegate) Spacing() int {
	return 0
}

func (d noteDelegate) Update(_ tea.Msg, _ *list.Model) tea.Cmd {
	return nil
}

func (d noteDelegate) Render(w io.Writer, m list.Model, index int, item list.Item) {
	i, ok := item.(noteItem)
	if !ok {
		return
	}

	check := " "
	if i.selected {
		check = "x"
	}
	size := ""
	if i.n.Size > 0 {
		size = " " + subtleStyle.Render("("+humanBytes(i.n.Size)+")")
	}
	tag := ""
	if i.tag != "" {
		tag = " " + i.tagStyle.Render("("+i.tag+")")
	}

	row := fmt.Sprintf("[%s] %s%s%s", checkStyle.Render(check), i.n.RelPath, size, tag)
	if index == m.Index() {
		row = cursorStyle.Render("> ") + row
	} else {
		row = "  " + row
	}

	fmt.Fprint(w, row)
}

type config struct {
	BaseURL   string
	LocalDir  string
	RemoteDir string
}

type screen int

const (
	screenConnecting screen = iota
	screenMain
	screenCopying
	screenOverwriteConfirm
	screenCleanConfirm
	screenDeleting
	screenDone
	screenErr
)

type model struct {
	cfg config

	notes     []note
	copyList  list.Model
	cleanList list.Model
	spinner   spinner.Model
	progress  progress.Model
	status    string
	width     int
	height    int
	activeTab int
	doneTitle string

	screen screen
	err    error

	work       []note
	copyIndex  int
	overwrites []note

	copied  []note
	skipped int
	failed  []string

	deleteIndex  int
	deleted      int
	deletedNotes []note
	deleteFail   []string
}

type copyResultMsg struct {
	n   note
	err error
}

type deleteResultMsg struct {
	n   note
	err error
}

type listResultMsg struct {
	notes []note
	err   error
}

func main() {
	cfg, err := parseConfig(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}

	m := model{
		cfg:      cfg,
		spinner:  newSpinner(),
		progress: newProgress(),
		screen:   screenConnecting,
	}

	if _, err := tea.NewProgram(m).Run(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
}

func parseConfig(args []string) (config, error) {
	baseURL := envOrDefault("CROSSPOINT_URL", defaultCrossPointURL)

	fs := flag.NewFlagSet("crosspoint-sync", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.StringVar(&baseURL, "url", baseURL, "CrossPoint base URL")
	fs.Usage = func() {
		fmt.Fprintf(os.Stdout, `Usage: crosspoint-sync [--url URL] [local_path] [remote_path]

One-way sync selected notes from CrossPoint to a local folder.

Arguments:
  local_path   Local folder to copy notes into.
               Default: .

  remote_path  CrossPoint notes folder exposed over WebDAV.
               Default: /notes

Environment:
  CROSSPOINT_URL   Default: http://crosspoint.local

Examples:
  crosspoint-sync
  crosspoint-sync "~/Documents/My Notes"
  crosspoint-sync --url http://192.168.1.42 "~/Documents/My Notes" "/notes"
`)
	}

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fs.Usage()
			os.Exit(0)
		}
		fs.Usage()
		return config{}, err
	}

	rest := fs.Args()
	if len(rest) > 2 {
		fs.Usage()
		return config{}, fmt.Errorf("too many arguments")
	}

	localDir := defaultLocalDir
	remoteDir := defaultRemoteDir
	if len(rest) >= 1 {
		localDir = rest[0]
	}
	if len(rest) >= 2 {
		remoteDir = rest[1]
	}

	localDir = expandTilde(localDir)
	remoteDir = normalizeRemotePath(remoteDir)
	if _, err := url.ParseRequestURI(baseURL); err != nil {
		return config{}, fmt.Errorf("invalid CrossPoint URL %q: %w", baseURL, err)
	}

	return config{
		BaseURL:   strings.TrimRight(baseURL, "/"),
		LocalDir:  localDir,
		RemoteDir: remoteDir,
	}, nil
}

func envOrDefault(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func expandTilde(p string) string {
	if p == "~" {
		if home, err := os.UserHomeDir(); err == nil {
			return home
		}
	}
	if strings.HasPrefix(p, "~/") {
		if home, err := os.UserHomeDir(); err == nil {
			return filepath.Join(home, p[2:])
		}
	}
	return p
}

func normalizeRemotePath(p string) string {
	p = strings.TrimSpace(p)
	if p == "" || p == "/" {
		return "/"
	}
	p = path.Clean("/" + strings.TrimPrefix(p, "/"))
	if p != "/" {
		p = strings.TrimRight(p, "/")
	}
	return p
}

func (m model) Init() tea.Cmd {
	if m.screen == screenConnecting {
		return tea.Batch(listNotesCmd(m.cfg), m.spinner.Tick)
	}
	return nil
}

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width = msg.Width
		m.height = msg.Height
		if len(m.notes) > 0 {
			m.copyList.SetSize(m.listWidth(), m.listHeight())
			m.cleanList.SetSize(m.listWidth(), m.listHeight())
		}
		m.progress.Width = m.progressWidth()
		return m, nil

	case tea.KeyMsg:
		switch m.screen {
		case screenConnecting:
			if msg.String() == "ctrl+c" || msg.String() == "q" || msg.String() == "esc" {
				return m, tea.Quit
			}
		case screenMain:
			return m.updateMain(msg)
		case screenOverwriteConfirm:
			return m.updateOverwriteConfirm(msg)
		case screenCleanConfirm:
			return m.updateCleanConfirm(msg)
		case screenDone, screenErr:
			switch msg.String() {
			case "enter", "q", "esc", "ctrl+c":
				if msg.String() == "enter" && m.screen == screenDone {
					m.refreshLists()
					m.screen = screenMain
					return m, nil
				}
				return m, tea.Quit
			}
		default:
			if msg.String() == "ctrl+c" {
				return m, tea.Quit
			}
		}

	case listResultMsg:
		if msg.err != nil {
			m.err = msg.err
			m.screen = screenErr
			return m, nil
		}
		if len(msg.notes) == 0 {
			m.err = fmt.Errorf("no notes found at %s%s", strings.TrimRight(m.cfg.BaseURL, "/"), m.cfg.RemoteDir)
			m.screen = screenErr
			return m, nil
		}
		m.notes = msg.notes
		m.copyList = newCopyList(msg.notes, m.cfg, m.listWidth(), m.listHeight())
		m.cleanList = newCleanList(msg.notes, m.cfg, m.listWidth(), m.listHeight())
		m.screen = screenMain
		return m, nil

	case copyResultMsg:
		if msg.err != nil {
			m.failed = append(m.failed, fmt.Sprintf("%s: %v", msg.n.RelPath, msg.err))
		} else {
			m.copied = append(m.copied, msg.n)
		}
		m.copyIndex++
		cmd := m.prepareNextCopy()
		return m, tea.Batch(cmd, m.setProgressPercent(m.copyIndex, len(m.work)))

	case deleteResultMsg:
		if msg.err != nil {
			m.deleteFail = append(m.deleteFail, fmt.Sprintf("%s: %v", msg.n.RelPath, msg.err))
		} else {
			m.deleted++
			m.deletedNotes = append(m.deletedNotes, msg.n)
		}
		m.deleteIndex++
		cmd := m.prepareNextDelete()
		return m, tea.Batch(cmd, m.setProgressPercent(m.deleteIndex, len(m.work)))

	case progress.FrameMsg:
		progressModel, cmd := m.progress.Update(msg)
		m.progress = progressModel.(progress.Model)
		return m, cmd

	case spinner.TickMsg:
		if m.screen == screenConnecting {
			var cmd tea.Cmd
			m.spinner, cmd = m.spinner.Update(msg)
			return m, cmd
		}
		return m, nil
	}

	return m, nil
}

func (m model) updateMain(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	activeList := m.activeList()
	if activeList.SettingFilter() {
		var cmd tea.Cmd
		activeList, cmd = activeList.Update(msg)
		m.setActiveList(activeList)
		return m, cmd
	}

	switch msg.String() {
	case "ctrl+c", "q", "esc":
		return m, tea.Quit
	case "tab", "right", "l":
		m.activeTab = (m.activeTab + 1) % 2
		m.status = ""
		return m, nil
	case "shift+tab", "left", "h":
		m.activeTab = (m.activeTab + 1) % 2
		m.status = ""
		return m, nil
	case " ":
		return m, m.toggleCurrentNote()
	case "a":
		return m, m.toggleAllNotes()
	case "n":
		return m, m.setAllNotes(false)
	case "enter":
		if m.activeTab == 0 {
			m.work = selectedNotesFromList(m.copyList)
			if len(m.work) == 0 {
				m.status = "Select at least one note before copying."
				return m, nil
			}
			m.overwrites = overwriteNotes(m.cfg.LocalDir, m.work)
			if len(m.overwrites) > 0 {
				m.screen = screenOverwriteConfirm
				return m, nil
			}
			cmd := m.beginCopy()
			return m, cmd
		}

		m.work = selectedNotesFromList(m.cleanList)
		if len(m.work) == 0 {
			m.status = "Select at least one note before cleaning."
			return m, nil
		}
		m.screen = screenCleanConfirm
		return m, nil
	}

	var cmd tea.Cmd
	activeList, cmd = activeList.Update(msg)
	m.setActiveList(activeList)
	return m, cmd
}

func (m model) updateOverwriteConfirm(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q", "esc":
		return m, tea.Quit
	case "y", "Y":
		cmd := m.beginCopy()
		return m, cmd
	case "n", "N":
		m.status = "Copy cancelled. Review the selected notes and press Enter to copy."
		m.screen = screenMain
		return m, nil
	}
	return m, nil
}

func (m model) updateCleanConfirm(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q", "esc":
		return m, tea.Quit
	case "y", "Y":
		cmd := m.beginClean()
		return m, cmd
	case "n", "N":
		m.status = "Clean cancelled. Review the selected notes and press Enter to delete."
		m.screen = screenMain
		return m, nil
	}
	return m, nil
}

func (m *model) prepareNextCopy() tea.Cmd {
	if m.copyIndex < len(m.work) {
		m.screen = screenCopying
		return copyCmd(m.cfg, m.work[m.copyIndex])
	}

	m.doneTitle = "Copy complete"
	m.screen = screenDone
	return nil
}

func (m *model) prepareNextDelete() tea.Cmd {
	if m.deleteIndex >= len(m.work) {
		m.doneTitle = "Clean complete"
		m.removeDeletedNotes()
		m.screen = screenDone
		return nil
	}
	m.screen = screenDeleting
	return deleteCmd(m.cfg, m.work[m.deleteIndex])
}

func (m model) View() string {
	var body string
	switch m.screen {
	case screenConnecting:
		body = renderPanel("Connecting", lipgloss.JoinVertical(
			lipgloss.Left,
			fmt.Sprintf("%s Getting file list from CrossPoint", m.spinner.View()),
			"",
			labelStyle.Render("Remote: ")+valueStyle.Render(m.cfg.BaseURL+m.cfg.RemoteDir),
			labelStyle.Render("Local:  ")+valueStyle.Render(m.cfg.LocalDir),
			"",
			helpStyle.Render("Press q to quit."),
		))
	case screenMain:
		body = m.viewMain()
	case screenCopying:
		body = renderPanel("Copying notes", lipgloss.JoinVertical(
			lipgloss.Left,
			m.progress.View(),
			m.progressLine(),
			"",
			m.currentCopyLine(),
		))
	case screenOverwriteConfirm:
		body = renderPanel("Confirm overwrites", lipgloss.JoinVertical(
			lipgloss.Left,
			warningStyle.Render("These selected notes already exist locally and will be overwritten:"),
			m.overwriteSummary(),
			"",
			helpStyle.Render("Press y to copy and overwrite, n to return to the list."),
		))
	case screenCleanConfirm:
		body = renderPanel("Confirm delete", lipgloss.JoinVertical(
			lipgloss.Left,
			warningStyle.Render("These notes will be deleted from CrossPoint:"),
			m.cleanSummary(),
			"",
			helpStyle.Render("Press y to delete, n to return to the list."),
		))
	case screenDeleting:
		body = renderPanel("Deleting notes", lipgloss.JoinVertical(
			lipgloss.Left,
			m.progress.View(),
			fmt.Sprintf("%d/%d processed", m.deleted+len(m.deleteFail), len(m.work)),
			"Current: "+m.work[m.deleteIndex].RelPath,
		))
	case screenDone:
		body = renderPanel(m.doneTitle, lipgloss.JoinVertical(lipgloss.Left, m.summary(), "", helpStyle.Render("Press Enter to return to tabs, q to quit.")))
	case screenErr:
		body = renderPanel("Error", lipgloss.JoinVertical(lipgloss.Left, errorStyle.Render(m.err.Error()), "", helpStyle.Render("Press Enter to exit.")))
	default:
		body = ""
	}
	return appStyle.Render(body)
}

func (m model) viewMain() string {
	var b strings.Builder
	fmt.Fprintln(&b, titleStyle.Render("CrossPoint Notes Sync"))
	fmt.Fprintf(&b, "%s %s\n", labelStyle.Render("Remote:"), valueStyle.Render(m.cfg.BaseURL+m.cfg.RemoteDir))
	fmt.Fprintf(&b, "%s  %s\n\n", labelStyle.Render("Local:"), valueStyle.Render(m.cfg.LocalDir))

	var tabContent strings.Builder
	if m.activeTab == 0 {
		fmt.Fprintln(&tabContent, m.copyList.View())
		fmt.Fprintf(&tabContent, "\n%s\n", subtleStyle.Render(fmt.Sprintf("%d notes. %d selected.", len(m.notes), selectedCountFromList(m.copyList))))
	} else {
		fmt.Fprintln(&tabContent, m.cleanList.View())
		fmt.Fprintf(&tabContent, "\n%s\n", subtleStyle.Render(fmt.Sprintf("%d notes. %d selected.", len(m.notes), selectedCountFromList(m.cleanList))))
	}
	if m.status != "" {
		fmt.Fprintf(&tabContent, "\n%s\n", statusStyle.Render(m.status))
	}
	fmt.Fprint(&b, m.tabbedWindow(tabContent.String()))
	fmt.Fprintf(&b, "\n%s\n", helpStyle.Width(m.tabContentWidth()).Render("tab switch  up/down move  space toggle  a all/none  n none  enter run  q quit"))

	return b.String()
}

func (m model) progressLine() string {
	return fmt.Sprintf("%s %d/%d   %s %d   %s %d   %s %d",
		labelStyle.Render("Processed:"),
		m.copyIndex,
		len(m.work),
		successStyle.Render("Copied:"),
		len(m.copied),
		warningStyle.Render("Skipped:"),
		m.skipped,
		errorStyle.Render("Failed:"),
		len(m.failed),
	)
}

func (m model) currentCopyLine() string {
	if m.copyIndex >= len(m.work) {
		return subtleStyle.Render("Finishing...")
	}
	return labelStyle.Render("Current: ") + valueStyle.Render(m.work[m.copyIndex].RelPath)
}

func (m model) summary() string {
	var b strings.Builder
	if m.doneTitle == "Clean complete" {
		fmt.Fprintf(&b, "%s %s   %s %s",
			successStyle.Render("Deleted"),
			valueStyle.Render(fmt.Sprintf("%d", m.deleted)),
			errorStyle.Render("Failed"),
			valueStyle.Render(fmt.Sprintf("%d", len(m.deleteFail))),
		)
		if len(m.deleteFail) > 0 {
			fmt.Fprintf(&b, "\n\n%s", errorStyle.Render("Delete failures:"))
			for _, failure := range m.deleteFail {
				fmt.Fprintf(&b, "\n- %s", failure)
			}
		}
		return b.String()
	}
	fmt.Fprintf(&b, "%s %s   %s %s",
		successStyle.Render("Copied"),
		valueStyle.Render(fmt.Sprintf("%d", len(m.copied))),
		errorStyle.Render("Failed"),
		valueStyle.Render(fmt.Sprintf("%d", len(m.failed))),
	)
	if len(m.failed) > 0 {
		fmt.Fprintf(&b, "\n\n%s", errorStyle.Render("Copy failures:"))
		for _, failure := range m.failed {
			fmt.Fprintf(&b, "\n- %s", failure)
		}
	}
	return b.String()
}

func (m model) overwriteSummary() string {
	var b strings.Builder
	limit := 12
	for i, n := range m.overwrites {
		if i >= limit {
			fmt.Fprintf(&b, "\n%s", subtleStyle.Render(fmt.Sprintf("...and %d more", len(m.overwrites)-limit)))
			break
		}
		fmt.Fprintf(&b, "\n- %s", valueStyle.Render(localNotePath(m.cfg.LocalDir, n.RelPath)))
	}
	return b.String()
}

func (m model) cleanSummary() string {
	var b strings.Builder
	limit := 12
	for i, n := range m.work {
		if i >= limit {
			fmt.Fprintf(&b, "\n%s", subtleStyle.Render(fmt.Sprintf("...and %d more", len(m.work)-limit)))
			break
		}
		fmt.Fprintf(&b, "\n- %s", valueStyle.Render(n.RelPath))
	}
	return b.String()
}

func renderPanel(title, body string) string {
	return lipgloss.JoinVertical(
		lipgloss.Left,
		titleStyle.Render(title),
		subtleStyle.Render(strings.Repeat("-", max(12, len(title)))),
		"",
		body,
	)
}

func tabBorderWithBottom(left, middle, right string) lipgloss.Border {
	border := lipgloss.RoundedBorder()
	border.BottomLeft = left
	border.Bottom = middle
	border.BottomRight = right
	return border
}

func (m model) tabBar() string {
	tabs := []string{"Copy", "Clean"}
	renderedTabs := make([]string, 0, len(tabs))
	for i, tab := range tabs {
		style := inactiveTabStyle
		isFirst := i == 0
		isLast := i == len(tabs)-1
		isActive := i == m.activeTab
		if isActive {
			style = activeTabStyle
		}
		border, _, _, _, _ := style.GetBorder()
		if isFirst && isActive {
			border.BottomLeft = "│"
		} else if isFirst && !isActive {
			border.BottomLeft = "├"
		} else if isLast && isActive {
			border.BottomRight = "└"
		} else if isLast && !isActive {
			border.BottomRight = "┴"
		}
		renderedTabs = append(renderedTabs, style.Border(border).Render(tab))
	}
	return lipgloss.JoinHorizontal(lipgloss.Top, renderedTabs...)
}

func (m model) tabbedWindow(content string) string {
	row := m.tabBar()
	contentWidth := max(1, m.tabContentWidth()-tabWindowStyle.GetHorizontalFrameSize())
	window := tabWindowStyle.Width(contentWidth).Render(content)
	if rowWidth, windowWidth := lipgloss.Width(row), lipgloss.Width(window); rowWidth < windowWidth {
		remaining := windowWidth - rowWidth
		if remaining == 1 {
			row += lipgloss.NewStyle().Foreground(highlightColor).Render("┐")
		} else {
			row += lipgloss.NewStyle().Foreground(highlightColor).Render(strings.Repeat("─", remaining-1) + "┐")
		}
	}
	return lipgloss.JoinVertical(lipgloss.Left, row, window)
}

func newCopyList(notes []note, cfg config, width, height int) list.Model {
	items := make([]list.Item, 0, len(notes))
	for _, n := range notes {
		exists := localFileExists(cfg.LocalDir, n.RelPath)
		item := noteItem{n: n, selected: !exists, localExists: exists}
		if exists {
			item.tag = "overwrite"
			item.tagStyle = warningStyle
		}
		items = append(items, item)
	}
	return newNoteList(items, width, height)
}

func newCleanList(notes []note, cfg config, width, height int) list.Model {
	items := make([]list.Item, 0, len(notes))
	for _, n := range notes {
		exists := localFileExists(cfg.LocalDir, n.RelPath)
		item := noteItem{n: n, selected: exists, localExists: exists}
		if exists {
			item.tag = "saved"
			item.tagStyle = successStyle
		}
		items = append(items, item)
	}
	return newNoteList(items, width, height)
}

func newNoteList(items []list.Item, width, height int) list.Model {
	l := list.New(items, noteDelegate{}, width, height)
	l.Title = "Notes"
	l.SetShowTitle(false)
	l.SetShowStatusBar(false)
	l.SetFilteringEnabled(false)
	l.SetShowHelp(false)
	l.SetShowPagination(false)
	l.SetStatusBarItemName("note", "notes")
	return l
}

func newSpinner() spinner.Model {
	s := spinner.New(spinner.WithSpinner(spinner.Dot), spinner.WithStyle(cursorStyle))
	return s
}

func newProgress() progress.Model {
	p := progress.New(progress.WithDefaultGradient())
	p.Width = 32
	return p
}

func (m model) listWidth() int {
	width := lipgloss.Width("4 notes. 4 selected.")
	for _, n := range m.notes {
		itemWidth := lipgloss.Width(n.RelPath) + 12
		if n.Size > 0 {
			itemWidth += lipgloss.Width(humanBytes(n.Size)) + 3
		}
		if localFileExists(m.cfg.LocalDir, n.RelPath) {
			itemWidth += lipgloss.Width("(overwrite)") + 1
		}
		width = max(width, itemWidth)
	}
	width = max(width, lipgloss.Width(m.tabBar()))
	width = max(width, 40)
	width = min(width, 68)
	if m.width > 0 {
		width = min(width, max(30, m.width-10))
	}
	return width
}

func (m model) listHeight() int {
	height := len(m.notes)
	if height < 1 {
		height = 1
	}
	if height > 12 {
		height = 12
	}
	if m.height > 0 {
		height = min(height, max(1, m.height-16))
	}
	return height
}

func (m model) tabContentWidth() int {
	return max(lipgloss.Width(m.tabBar()), m.listWidth())
}

func (m model) progressWidth() int {
	if m.width <= 0 {
		return 32
	}
	width := m.width - 20
	if width > 48 {
		return 48
	}
	if width < 16 {
		return 16
	}
	return width
}

func (m *model) setProgressPercent(current, total int) tea.Cmd {
	if total <= 0 {
		return m.progress.SetPercent(0)
	}
	if current < 0 {
		current = 0
	}
	if current > total {
		current = total
	}
	return m.progress.SetPercent(float64(current) / float64(total))
}

func (m *model) toggleCurrentNote() tea.Cmd {
	activeList := m.activeList()
	index := activeList.GlobalIndex()
	if index < 0 || index >= len(activeList.Items()) {
		return nil
	}
	item, ok := activeList.Items()[index].(noteItem)
	if !ok {
		return nil
	}
	item.selected = !item.selected
	m.status = ""
	cmd := activeList.SetItem(index, item)
	m.setActiveList(activeList)
	return cmd
}

func (m *model) toggleAllNotes() tea.Cmd {
	activeList := m.activeList()
	allSelected := selectedCountFromList(activeList) == len(activeList.Items())
	return m.setAllNotes(!allSelected)
}

func (m *model) setAllNotes(selected bool) tea.Cmd {
	activeList := m.activeList()
	items := activeList.Items()
	next := make([]list.Item, 0, len(items))
	for _, item := range items {
		noteItem, ok := item.(noteItem)
		if !ok {
			next = append(next, item)
			continue
		}
		noteItem.selected = selected
		next = append(next, noteItem)
	}
	m.status = ""
	cmd := activeList.SetItems(next)
	m.setActiveList(activeList)
	return cmd
}

func (m *model) beginCopy() tea.Cmd {
	m.copyIndex = 0
	m.copied = nil
	m.skipped = 0
	m.failed = nil
	m.deleteIndex = 0
	m.deleted = 0
	m.deletedNotes = nil
	m.deleteFail = nil
	m.progress = newProgress()
	m.progress.Width = m.progressWidth()
	m.screen = screenCopying
	cmd := m.prepareNextCopy()
	return tea.Batch(cmd, m.setProgressPercent(0, len(m.work)))
}

func (m *model) beginClean() tea.Cmd {
	m.deleteIndex = 0
	m.deleted = 0
	m.deletedNotes = nil
	m.deleteFail = nil
	m.copied = nil
	m.failed = nil
	m.progress = newProgress()
	m.progress.Width = m.progressWidth()
	m.screen = screenDeleting
	cmd := m.prepareNextDelete()
	return tea.Batch(cmd, m.setProgressPercent(0, len(m.work)))
}

func (m model) activeList() list.Model {
	if m.activeTab == 0 {
		return m.copyList
	}
	return m.cleanList
}

func (m *model) setActiveList(activeList list.Model) {
	if m.activeTab == 0 {
		m.copyList = activeList
	} else {
		m.cleanList = activeList
	}
}

func (m *model) refreshLists() {
	m.copyList = newCopyList(m.notes, m.cfg, m.listWidth(), m.listHeight())
	m.cleanList = newCleanList(m.notes, m.cfg, m.listWidth(), m.listHeight())
	m.status = ""
}

func (m *model) removeDeletedNotes() {
	if len(m.deletedNotes) == 0 {
		return
	}
	deleted := make(map[string]struct{}, len(m.deletedNotes))
	for _, n := range m.deletedNotes {
		deleted[n.RemotePath] = struct{}{}
	}
	remaining := m.notes[:0]
	for _, n := range m.notes {
		if _, ok := deleted[n.RemotePath]; !ok {
			remaining = append(remaining, n)
		}
	}
	m.notes = remaining
}

func selectedNotesFromList(noteList list.Model) []note {
	var out []note
	for _, item := range noteList.Items() {
		noteItem, ok := item.(noteItem)
		if ok && noteItem.selected {
			out = append(out, noteItem.n)
		}
	}
	return out
}

func selectedCountFromList(noteList list.Model) int {
	return len(selectedNotesFromList(noteList))
}

func overwriteNotes(localDir string, notes []note) []note {
	var out []note
	for _, n := range notes {
		if localFileExists(localDir, n.RelPath) {
			out = append(out, n)
		}
	}
	return out
}

func localFileExists(localDir, rel string) bool {
	_, err := os.Stat(localNotePath(localDir, rel))
	return err == nil
}

func listNotesCmd(cfg config) tea.Cmd {
	return func() tea.Msg {
		client := &http.Client{Timeout: 45 * time.Second}
		ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
		defer cancel()

		notes, err := listNotes(ctx, client, cfg.BaseURL, cfg.RemoteDir)
		if err != nil {
			return listResultMsg{err: fmt.Errorf("could not list notes at %s%s: %w", strings.TrimRight(cfg.BaseURL, "/"), cfg.RemoteDir, err)}
		}
		return listResultMsg{notes: notes}
	}
}

func listNotes(ctx context.Context, client *http.Client, baseURL, remoteDir string) ([]note, error) {
	remoteDir = normalizeRemotePath(remoteDir)
	seenDirs := map[string]bool{}
	var notes []note

	var walk func(string) error
	walk = func(dir string) error {
		dir = normalizeRemotePath(dir)
		if seenDirs[dir] {
			return nil
		}
		seenDirs[dir] = true

		entries, err := propfind(ctx, client, baseURL, dir)
		if err != nil {
			return err
		}

		for _, entry := range entries {
			entry.Path = normalizeRemotePath(entry.Path)
			if entry.Path == dir {
				continue
			}
			if entry.IsDir {
				if err := walk(entry.Path); err != nil {
					return err
				}
				continue
			}

			rel := remoteRelativePath(remoteDir, entry.Path)
			if rel == "" {
				continue
			}
			notes = append(notes, note{
				RelPath:    rel,
				RemotePath: entry.Path,
				Size:       entry.Size,
			})
		}
		return nil
	}

	if err := walk(remoteDir); err != nil {
		return nil, err
	}

	sort.Slice(notes, func(i, j int) bool {
		return strings.ToLower(notes[i].RelPath) < strings.ToLower(notes[j].RelPath)
	})
	return notes, nil
}

type davEntry struct {
	Path  string
	IsDir bool
	Size  int64
}

type davResponse struct {
	Href     string        `xml:"href"`
	PropStat []davPropStat `xml:"propstat"`
}

type davPropStat struct {
	Prop davProp `xml:"prop"`
}

type davProp struct {
	ResourceType davResourceType `xml:"resourcetype"`
	ContentLen   int64           `xml:"getcontentlength"`
}

type davResourceType struct {
	Collection *struct{} `xml:"collection"`
}

func propfind(ctx context.Context, client *http.Client, baseURL, remotePath string) ([]davEntry, error) {
	req, err := http.NewRequestWithContext(ctx, "PROPFIND", webdavURL(baseURL, remotePath), nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Depth", "1")

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		return nil, fmt.Errorf("PROPFIND %s returned %s: %s", remotePath, resp.Status, strings.TrimSpace(string(body)))
	}

	decoder := xml.NewDecoder(resp.Body)
	var entries []davEntry
	for {
		tok, err := decoder.Token()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, err
		}

		start, ok := tok.(xml.StartElement)
		if !ok || start.Name.Local != "response" {
			continue
		}

		var r davResponse
		if err := decoder.DecodeElement(&r, &start); err != nil {
			return nil, err
		}
		entryPath, err := hrefPath(r.Href)
		if err != nil {
			return nil, err
		}
		entries = append(entries, davEntry{
			Path:  entryPath,
			IsDir: responseIsDir(r),
			Size:  responseSize(r),
		})
	}

	return entries, nil
}

func hrefPath(href string) (string, error) {
	if href == "" {
		return "", fmt.Errorf("empty WebDAV href")
	}
	if parsed, err := url.Parse(href); err == nil && parsed.Scheme != "" {
		href = parsed.Path
	}
	decoded, err := url.PathUnescape(href)
	if err != nil {
		return "", err
	}
	return normalizeRemotePath(decoded), nil
}

func responseIsDir(r davResponse) bool {
	for _, stat := range r.PropStat {
		if stat.Prop.ResourceType.Collection != nil {
			return true
		}
	}
	return false
}

func responseSize(r davResponse) int64 {
	for _, stat := range r.PropStat {
		if stat.Prop.ContentLen > 0 {
			return stat.Prop.ContentLen
		}
	}
	return 0
}

func remoteRelativePath(root, child string) string {
	root = normalizeRemotePath(root)
	child = normalizeRemotePath(child)
	if root == "/" {
		return cleanRelative(strings.TrimPrefix(child, "/"))
	}
	prefix := root + "/"
	if !strings.HasPrefix(child, prefix) {
		return ""
	}
	return cleanRelative(strings.TrimPrefix(child, prefix))
}

func cleanRelative(rel string) string {
	rel = strings.TrimPrefix(path.Clean("/"+rel), "/")
	if rel == "." || rel == ".." || strings.HasPrefix(rel, "../") {
		return ""
	}
	return rel
}

func copyCmd(cfg config, n note) tea.Cmd {
	return func() tea.Msg {
		return copyResultMsg{n: n, err: copyNote(cfg, n)}
	}
}

func deleteCmd(cfg config, n note) tea.Cmd {
	return func() tea.Msg {
		return deleteResultMsg{n: n, err: deleteNote(cfg, n)}
	}
}

func copyNote(cfg config, n note) error {
	localPath := localNotePath(cfg.LocalDir, n.RelPath)
	if err := os.MkdirAll(filepath.Dir(localPath), 0755); err != nil {
		return err
	}

	req, err := http.NewRequest(http.MethodGet, downloadURL(cfg.BaseURL, n.RemotePath), nil)
	if err != nil {
		return err
	}

	client := &http.Client{Timeout: 60 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		return fmt.Errorf("GET returned %s: %s", resp.Status, strings.TrimSpace(string(body)))
	}

	tmp, err := os.CreateTemp(filepath.Dir(localPath), "."+filepath.Base(localPath)+".tmp-")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	removeTmp := true
	defer func() {
		if removeTmp {
			_ = os.Remove(tmpPath)
		}
	}()

	if _, err := io.Copy(tmp, resp.Body); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}

	if err := os.Rename(tmpPath, localPath); err != nil {
		return err
	}
	removeTmp = false
	return nil
}

func deleteNote(cfg config, n note) error {
	req, err := http.NewRequest(http.MethodDelete, webdavURL(cfg.BaseURL, n.RemotePath), nil)
	if err != nil {
		return err
	}

	client := &http.Client{Timeout: 30 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNoContent || resp.StatusCode == http.StatusOK || resp.StatusCode == http.StatusAccepted {
		return nil
	}
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	return fmt.Errorf("DELETE returned %s: %s", resp.Status, strings.TrimSpace(string(body)))
}

func localNotePath(localDir, rel string) string {
	return filepath.Join(localDir, filepath.FromSlash(cleanRelative(rel)))
}

func webdavURL(baseURL, remotePath string) string {
	return strings.TrimRight(baseURL, "/") + escapePath(normalizeRemotePath(remotePath))
}

func downloadURL(baseURL, remotePath string) string {
	values := url.Values{}
	values.Set("path", normalizeRemotePath(remotePath))
	return strings.TrimRight(baseURL, "/") + "/download?" + values.Encode()
}

func escapePath(p string) string {
	if p == "/" {
		return "/"
	}
	parts := strings.Split(strings.TrimPrefix(p, "/"), "/")
	for i, part := range parts {
		parts[i] = url.PathEscape(part)
	}
	return "/" + strings.Join(parts, "/")
}

func humanBytes(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	value := float64(n)
	for _, suffix := range []string{"KB", "MB", "GB"} {
		value /= unit
		if value < unit {
			return fmt.Sprintf("%.1f %s", value, suffix)
		}
	}
	return fmt.Sprintf("%.1f TB", value/unit)
}
