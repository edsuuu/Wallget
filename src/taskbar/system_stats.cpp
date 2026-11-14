#include "taskbar/taskbar.h"
#include "common/log.h"
#include "common/registry.h"
#include "common/xaml_tree.h"
#include "wallpaper/palette.h"
#include "wallpaper/sensors.h"

#include <dxgi.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

namespace Media = winrt::Windows::UI::Xaml::Media;
namespace Shapes = winrt::Windows::UI::Xaml::Shapes;

// Anéis de CPU, RAM e GPU na barra (sem a rede: pedido dele), à esquerda da bandeja, como os do papel de parede em
// miniatura: anel com a % dentro e, ao lado, o nome e a temperatura (ou os GB). Sem espaço para os três (bandeja cheia),
// somem. Não pausa em jogo de tela cheia (pedido dele).
constexpr double kRing = 32, kThickness = 2.5, kSide = 42, kInnerGap = 5, kSpacing = 8, kEdgeGap = 8;
constexpr double kItem = kRing + kInnerGap + kSide;

struct Gauge {
    Shapes::Ellipse arc{nullptr};
    Controls::TextBlock value{nullptr}, top{nullptr}, bottom{nullptr};
    Controls::StackPanel root{nullptr};
};

struct StatsPanel {
    Controls::StackPanel root{nullptr};
    Gauge gauges[3];
    winrt::weak_ref<FrameworkElement> tray;
    double margin = -1;
    bool gone = false;
};

struct Reading {
    double percents[3]{};
    std::wstring bottoms[3];
};

// Tudo na thread do XAML da barra (as barras de todos os monitores dividem a mesma).
static thread_local std::vector<std::shared_ptr<StatsPanel>> g_panels;
static thread_local DispatcherTimer g_timer{nullptr};
static thread_local std::unique_ptr<SystemStats> g_stats;
static thread_local Reading g_reading;
static thread_local int g_ticks, g_palette = -1;
static thread_local bool g_enabled = true;

static Media::SolidColorBrush Brush(Ink ink, float gray, float alpha) {
    auto c = Paint(ink, gray, alpha);
    return Media::SolidColorBrush(Color{BYTE(c.a * 255 + 0.5f), BYTE(c.r * 255 + 0.5f), BYTE(c.g * 255 + 0.5f), BYTE(c.b * 255 + 0.5f)});
}

static Controls::TextBlock Text(double size, Media::Brush const& color) {
    Controls::TextBlock text;
    text.FontSize(size);
    text.Foreground(color);
    text.TextWrapping(TextWrapping::NoWrap);
    text.VerticalAlignment(VerticalAlignment::Center);
    return text;
}

static void SetText(Controls::TextBlock const& block, std::wstring const& text) {
    if (block.Text() != text) block.Text(text);
}

// Um anel: trilho e arco em CorAnel (o arco sai do topo no sentido horário), a % no meio em CorTexto e o nome e o
// detalhe ao lado em CorTextoSuave, nos brilhos do papel de parede.
static Gauge MakeGauge() {
    Gauge g;
    auto ellipse = [](Media::Brush const& stroke) {
        Shapes::Ellipse e;
        e.Width(kRing);
        e.Height(kRing);
        e.StrokeThickness(kThickness);
        e.Stroke(stroke);
        return e;
    };
    auto track = ellipse(Brush(Ink::Accent, 1, 0.18f));
    g.arc = ellipse(Brush(Ink::Accent, 0.95f, 0.97f));
    g.arc.StrokeDashCap(Media::PenLineCap::Round);
    Media::RotateTransform turn;
    turn.Angle(-90);  // o traço da elipse começa às 3 horas
    g.arc.RenderTransformOrigin({0.5f, 0.5f});
    g.arc.RenderTransform(turn);

    g.value = Text(9.5, Brush(Ink::Text, 0.95f, 0.97f));
    g.value.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    g.value.HorizontalAlignment(HorizontalAlignment::Center);
    Controls::Grid ring;
    ring.Width(kRing);
    ring.Height(kRing);
    for (UIElement e : {UIElement(track), UIElement(g.arc), UIElement(g.value)}) ring.Children().Append(e);

    g.top = Text(10.5, Brush(Ink::Soft, 0.69f, 0.95f));
    g.bottom = Text(11.5, Brush(Ink::Soft, 0.82f, 0.95f));
    Controls::StackPanel side;
    side.Width(kSide);
    side.VerticalAlignment(VerticalAlignment::Center);
    side.Children().Append(g.top);
    side.Children().Append(g.bottom);

    g.root = Controls::StackPanel();
    g.root.Orientation(Controls::Orientation::Horizontal);
    g.root.Spacing(kInnerGap);
    g.root.Children().Append(ring);
    g.root.Children().Append(side);
    return g;
}

// Arco de 0,5 a 99,9% (como no papel de parede). O StrokeDashArray conta em espessuras do traço.
static void SetPercent(Shapes::Ellipse const& arc, double percent) {
    double circle = 3.14159265358979 * (kRing - kThickness) / kThickness;
    Media::DoubleCollection dash;
    dash.Append(std::clamp(percent, 0.5, 99.9) / 100 * circle);
    dash.Append(circle);
    arc.StrokeDashArray(dash);
}

static void ShowReading(StatsPanel& p) {
    for (int i = 0; i < 3; i++) {
        auto& g = p.gauges[i];
        SetPercent(g.arc, g_reading.percents[i]);
        SetText(g.value, std::to_wstring(lround(g_reading.percents[i])) + L"%");
        SetText(g.bottom, g_reading.bottoms[i]);
    }
}

static std::shared_ptr<StatsPanel> MakePanel() {
    auto p = std::make_shared<StatsPanel>();
    p->root = Controls::StackPanel();
    p->root.Name(L"TemaDesempenho");
    p->root.Orientation(Controls::Orientation::Horizontal);
    p->root.Spacing(kSpacing);
    p->root.HorizontalAlignment(HorizontalAlignment::Right);
    p->root.VerticalAlignment(VerticalAlignment::Center);
    p->root.IsHitTestVisible(false);  // a barra continua recebendo o clique direito ali
    p->root.Visibility(Visibility::Collapsed);
    const wchar_t* names[3] = {L"CPU", L"RAM", L"GPU"};
    for (int i = 0; i < 3; i++) {
        p->gauges[i] = MakeGauge();
        p->gauges[i].top.Text(names[i]);
        p->root.Children().Append(p->gauges[i].root);
    }
    ShowReading(*p);

    std::weak_ptr<StatsPanel> weak = p;
    p->root.Loaded([weak](auto&&, auto&&) {
        if (auto p = weak.lock()) p->gone = false;
    });
    p->root.Unloaded([weak](auto&&, auto&&) {
        if (auto p = weak.lock()) p->gone = true;
    });
    return p;
}

// Primeiro elemento da bandeja (classe SystemTray.*) na ilha da barra, em profundidade: o quadro dela.
static FrameworkElement FindTray(DependencyObject const& root, int depth) {
    for (int i = 0, count = VisualTreeHelper::GetChildrenCount(root); i < count; i++) {
        auto child = VisualTreeHelper::GetChild(root, i);
        if (ClassOf(child).starts_with(L"SystemTray.")) return child.try_as<FrameworkElement>();
        if (depth > 1)
            if (auto found = FindTray(child, depth - 1)) return found;
    }
    return nullptr;
}

// Borda esquerda da bandeja na grade, em DIPs (sem achar, a borda direita da barra).
static double TrayLeft(StatsPanel& p, FrameworkElement const& grid) {
    auto tray = p.tray.get();
    if (!tray || !tray.IsLoaded()) {
        auto root = grid.XamlRoot() ? grid.XamlRoot().Content() : nullptr;
        tray = root ? FindTray(root, 12) : nullptr;
        p.tray = tray ? winrt::make_weak(tray) : winrt::weak_ref<FrameworkElement>{};
        if (tray) Log(L"desempenho: bandeja %s em %.0f", ClassOf(tray).c_str(), tray.TransformToVisual(grid).TransformPoint({0, 0}).X);
    }
    return tray ? tray.TransformToVisual(grid).TransformPoint({0, 0}).X : grid.ActualWidth();
}

// Borda direita do último botão da barra (apps), em DIPs.
static double ButtonsRight(FrameworkElement const& grid) {
    auto repeater = FindDescendant(grid, L"TaskbarFrameRepeater", 4);
    double right = 0;
    for (int i = 0, count = repeater ? VisualTreeHelper::GetChildrenCount(repeater) : 0; i < count; i++) {
        auto item = VisualTreeHelper::GetChild(repeater, i).try_as<FrameworkElement>();
        if (item && item.ActualWidth() > 0 && item.Visibility() == Visibility::Visible)
            right = std::max(right, item.TransformToVisual(grid).TransformPoint({0, 0}).X + item.ActualWidth());
    }
    return right;
}

// Uma leitura de tudo, mesma conta do papel de parede.
static void Read() {
    if (!g_stats) {
        LUID luid{};
        winrt::com_ptr<IDXGIFactory1> factory;
        winrt::com_ptr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))) && SUCCEEDED(factory->EnumAdapters1(0, adapter.put())) &&
            SUCCEEDED(adapter->GetDesc1(&desc)))
            luid = desc.AdapterLuid;
        g_stats = std::make_unique<SystemStats>(luid);
    }
    g_stats->Read();
    auto temperature = [](double c) { return c < 0 ? std::wstring(L"--°") : std::to_wstring(lround(c)) + L" °C"; };
    wchar_t ram[16];
    swprintf_s(ram, L"%.1f GB", g_stats->ramGb);
    for (auto& c : ram)
        if (c == L'.') c = L',';

    g_reading.percents[0] = g_stats->cpu;
    g_reading.percents[1] = g_stats->ram;
    g_reading.percents[2] = g_stats->gpu;
    g_reading.bottoms[0] = temperature(g_stats->cpuTemperature);
    g_reading.bottoms[1] = ram;
    g_reading.bottoms[2] = temperature(g_stats->gpuTemperature);
}

// A cada 250 ms acerta o lugar e quantos anéis cabem entre o último app e a bandeja; a cada segundo lê os sensores.
// Sem olhar tela cheia: em jogo continua lendo.
static void Tick() {
    std::erase_if(g_panels, [](auto const& p) { return p->gone; });
    bool read = g_enabled && !g_panels.empty() && g_ticks++ % 4 == 0;
    if (read) Read();

    for (auto const& p : g_panels) {
        auto grid = ParentOf(p->root);
        if (!grid) continue;

        double tray = TrayLeft(*p, grid), room = tray - ButtonsRight(grid) - 2 * kEdgeGap;
        bool fits = g_enabled && room >= 3 * kItem + 2 * kSpacing;
        p->root.Visibility(fits ? Visibility::Visible : Visibility::Collapsed);
        if (!fits) continue;

        double margin = std::max(0.0, grid.ActualWidth() - tray + kEdgeGap);
        if (margin != p->margin) p->root.Margin({0, 0, p->margin = margin, 0});
        if (read) ShowReading(*p);
    }
}

static void PutPanel(Controls::Grid const& grid) {
    auto panel = MakePanel();
    Controls::Grid::SetColumnSpan(panel->root, std::max(1, int(grid.ColumnDefinitions().Size())));
    Controls::Grid::SetRowSpan(panel->root, std::max(1, int(grid.RowDefinitions().Size())));
    grid.Children().Append(panel->root);
    g_panels.push_back(panel);
}

// A cada segundo, junto com o resto da barra: Desempenho (0 esconde) e, mudando as cores do tema, painéis novos.
void RefreshSystemStats() {
    g_enabled = Enabled(L"Desempenho");
    if (PaletteVersion() == g_palette) return;

    g_palette = PaletteVersion();
    for (auto const& p : std::exchange(g_panels, {})) {
        auto grid = ParentOf(p->root).try_as<Controls::Grid>();
        if (!grid) continue;

        uint32_t index;
        if (grid.Children().IndexOf(p->root, index)) grid.Children().RemoveAt(index);
        PutPanel(grid);
    }
}

// Põe os anéis no Grid de cima da barra, uma vez por barra (como o player), e liga o timer.
void AddSystemStats(FrameworkElement const& frame) {
    auto found = FindDescendant(frame, L"RootGrid", 3);
    auto grid = found ? found.try_as<Controls::Grid>() : nullptr;
    if (!grid) return;

    RefreshSystemStats();
    if (auto old = FindDescendant(grid, L"TemaDesempenho", 1)) {
        if (std::any_of(g_panels.begin(), g_panels.end(), [&](auto const& p) { return p->root == old; })) return;
        uint32_t index;
        if (grid.Children().IndexOf(old, index)) grid.Children().RemoveAt(index);
    }
    PutPanel(grid);

    if (g_timer) return;
    g_timer = DispatcherTimer();
    g_timer.Interval(std::chrono::milliseconds(250));
    g_timer.Tick([](auto&&, auto&&) {
        thread_local HRESULT last = S_OK;
        try {
            Tick();
            last = S_OK;
        } catch (...) {
            HRESULT error = winrt::to_hresult();
            if (error != last) Log(L"erro no desempenho: %08X", error);
            last = error;
        }
    });
    g_timer.Start();
}
