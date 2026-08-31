// ============================================================================
// src/app/pebble_drishya_ml.cpp — Pebble Intelligence Dashboard
// ============================================================================
// Full Drishya-driven ML showcase. Layout entirely via akruti layout engine —
// no manual coordinate math. Four live panels (KMeans, Regression, NN, Solvers)
// each render a rekha::Figure into their Drishya-allocated box.
// Sidebar: stat tiles + sparklines. Bottom: per-solver sparklines.
// ============================================================================
#define SOKOL_NO_DEPRECATED
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wenum-enum-conversion"
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#pragma clang diagnostic pop

#include "drishya/drishya.hpp"
#include "rekha/rekha.hpp"
#include "manas/manas.hpp"
#include "kalpa/kalpa.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace {

constexpr int   W  = 1280;
constexpr int   H  = 800;
constexpr float DT = 1.0f / 60.0f;

// ─────────────────────────────────────────────────────────────────────────────
// Shaders
// ─────────────────────────────────────────────────────────────────────────────
const char* VS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In  { float2 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };\n"
    "struct Out { float4 pos [[position]]; float4 col; };\n"
    "vertex Out vs(In in [[stage_in]]) { Out o; o.pos=float4(in.pos,0,1); o.col=in.col; return o; }\n";
const char* FS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In { float4 col; };\n"
    "fragment float4 fs(In in [[stage_in]]) { return in.col; }\n";
const char* VS_GLSL =
    "#version 330\nlayout(location=0) in vec2 pos; layout(location=1) in vec4 col;\n"
    "out vec4 v_col; void main(){ v_col=col; gl_Position=vec4(pos,0,1); }\n";
const char* FS_GLSL =
    "#version 330\nin vec4 v_col; out vec4 c;\nvoid main(){ c=v_col; }\n";

// ─────────────────────────────────────────────────────────────────────────────
// Palette
// ─────────────────────────────────────────────────────────────────────────────
namespace pal {
    constexpr uint32_t bg       = 0xFF080810u;
    constexpr uint32_t panel    = 0xFF111120u;
    constexpr uint32_t sidebar  = 0xFF0D0D1Au;
    constexpr uint32_t titlebar = 0xFF0A0A16u;
    constexpr uint32_t text_c   = 0xFFE1E8F8u;
    constexpr uint32_t dim      = 0xFF7880A0u;
    constexpr uint32_t accent   = 0xFF2EB8FFu;
    constexpr uint32_t green    = 0xFF38E68Au;
    constexpr uint32_t orange   = 0xFFFF9938u;
    constexpr uint32_t red      = 0xFFFF4848u;
    constexpr uint32_t purple   = 0xFFCC66FFu;
    constexpr uint32_t yellow   = 0xFFFFE038u;
    constexpr uint32_t teal     = 0xFF1AE5CCu;
    constexpr std::array<uint32_t,5> solver = {accent,green,orange,purple,yellow};

    constexpr kalpana::Color k_accent  {0.18f,0.72f,1.00f,1.0f};
    constexpr kalpana::Color k_green   {0.22f,0.90f,0.55f,1.0f};
    constexpr kalpana::Color k_orange  {1.00f,0.60f,0.22f,1.0f};
    constexpr kalpana::Color k_red     {1.00f,0.28f,0.28f,1.0f};
    constexpr kalpana::Color k_purple  {0.80f,0.40f,1.00f,1.0f};
    constexpr kalpana::Color k_yellow  {1.00f,0.88f,0.22f,1.0f};
    constexpr std::array<kalpana::Color,5> k_solver = {k_accent,k_green,k_orange,k_purple,k_yellow};
    constexpr std::array<kalpana::Color,3> k_cluster = {
        kalpana::Color{0.20f,0.72f,1.00f,0.90f},
        kalpana::Color{1.00f,0.55f,0.22f,0.90f},
        kalpana::Color{0.78f,0.35f,0.98f,0.90f},
    };
} // namespace pal

// ─────────────────────────────────────────────────────────────────────────────
// ML state
// ─────────────────────────────────────────────────────────────────────────────
struct MLState {
    float t = 0.0f;
    int   frame = 0;
    bool  paused = false;

    static constexpr int KM_N=120, KM_K=3;
    std::vector<float> km_x, km_y;
    std::vector<int>   km_labels;
    std::array<float,KM_K> km_cx{}, km_cy{};
    float km_inertia = 0.0f;

    static constexpr int LR_N=60;
    std::vector<float> lr_x, lr_y, lr_pred_x, lr_pred_y, lr_resid;
    float lr_r2 = 0.0f;

    manas::nn::Sequential nn_model{};
    manas::nn::Adam<>     nn_opt{0.005f};
    static constexpr int  NN_HIST=200;
    std::vector<float>    nn_loss_hist;
    float                 nn_loss_cur = 1.0f;
    int                   nn_epoch = 0;
    bool                  nn_init = false;
    float                 nn_acc = 0.0f;

    static constexpr int NUM_SOLVERS=5, KALPA_HIST=200, SPARK=48;
    std::array<std::vector<float>,NUM_SOLVERS> solver_hist{};
    std::array<std::vector<float>,NUM_SOLVERS> solver_spark{};
    std::array<int,  NUM_SOLVERS> solver_iters{};
    std::array<float,NUM_SOLVERS> solver_best{1e9f,1e9f,1e9f,1e9f,1e9f};
};

// ─────────────────────────────────────────────────────────────────────────────
// ML update functions
// ─────────────────────────────────────────────────────────────────────────────
static void update_kmeans(MLState& s) {
    const int N=MLState::KM_N, K=MLState::KM_K;
    s.km_x.resize(N); s.km_y.resize(N);
    const std::array<float,MLState::KM_K> cx = {
        22.0f+14.0f*std::cos(s.t*0.10f), 55.0f+11.0f*std::sin(s.t*0.08f+1.0f),
        82.0f+ 9.0f*std::cos(s.t*0.06f+2.1f)};
    const std::array<float,MLState::KM_K> cy = {
        72.0f+11.0f*std::sin(s.t*0.09f), 28.0f+13.0f*std::cos(s.t*0.11f+0.5f),
        68.0f+ 9.0f*std::sin(s.t*0.07f+1.6f)};
    for (int i=0;i<N;++i) {
        const int k=i%K;
        const float ang=static_cast<float>(i)*2.39996f;
        const float r=7.0f+5.0f*std::sin(s.t*1.2f+static_cast<float>(i)*0.37f);
        s.km_x[i]=std::clamp(cx[k]+r*std::cos(ang),2.0f,98.0f);
        s.km_y[i]=std::clamp(cy[k]+r*std::sin(ang),2.0f,98.0f);
    }
    ts::tensor<float> X({size_t(N),size_t(2)});
    for (int i=0;i<N;++i){X(size_t(i),size_t(0))=s.km_x[i];X(size_t(i),size_t(1))=s.km_y[i];}
    manas::ml::KMeans<manas::ml::KMeansPPInit> km(K); km.fit(X);
    s.km_labels=km.labels();
    const auto& c=km.cluster_centers(); s.km_inertia=0;
    for (int i=0;i<N;++i){const int k=s.km_labels[i];
        const float dx=s.km_x[i]-c(std::vector<size_t>{size_t(k),size_t(0)});
        const float dy=s.km_y[i]-c(std::vector<size_t>{size_t(k),size_t(1)});
        s.km_inertia+=dx*dx+dy*dy;}
    for (int k=0;k<K;++k){
        s.km_cx[k]=c(std::vector<size_t>{size_t(k),size_t(0)});
        s.km_cy[k]=c(std::vector<size_t>{size_t(k),size_t(1)});}
}

static void update_regression(MLState& s) {
    const int N=MLState::LR_N;
    s.lr_x.resize(N);s.lr_y.resize(N);
    for (int i=0;i<N;++i){
        const float x=static_cast<float>(i)/static_cast<float>(N-1)*100.0f;
        s.lr_x[i]=x;
        s.lr_y[i]=std::clamp(50.0f+28.0f*std::sin(x*0.065f+s.t*0.45f)
                   +8.0f*std::sin(x*0.14f+s.t*0.3f)+9.0f*(std::sin(static_cast<float>(i)*1.73f)-0.3f),3.0f,97.0f);}
    const float sc=1.0f/100.0f;
    ts::tensor<float> X({size_t(N),size_t(3)}),Y({size_t(N)});
    for (int i=0;i<N;++i){const float x=s.lr_x[i]*sc;
        X(size_t(i),size_t(0))=x;X(size_t(i),size_t(1))=x*x;X(size_t(i),size_t(2))=x*x*x;Y(size_t(i))=s.lr_y[i]*sc;}
    manas::ml::LinearRegression<> lr; lr.fit(X,Y);
    const int M=60; s.lr_pred_x.resize(M);s.lr_pred_y.resize(M);
    ts::tensor<float> Xp({size_t(M),size_t(3)});
    for (int i=0;i<M;++i){const float x=static_cast<float>(i)/static_cast<float>(M-1);
        Xp(size_t(i),size_t(0))=x;Xp(size_t(i),size_t(1))=x*x;Xp(size_t(i),size_t(2))=x*x*x;s.lr_pred_x[i]=x*100.0f;}
    const auto pred=lr.predict(Xp);
    for (int i=0;i<M;++i) s.lr_pred_y[i]=std::clamp(pred(std::vector<size_t>{size_t(i)})*100.0f,0.0f,100.0f);
    const auto tp=lr.predict(X);
    float ss_res=0,ss_tot=0,my=0;
    for (int i=0;i<N;++i) my+=s.lr_y[i]; my/=N;
    s.lr_resid.resize(N);
    for (int i=0;i<N;++i){const float yh=tp(std::vector<size_t>{size_t(i)})*100.0f;
        s.lr_resid[i]=s.lr_y[i]-yh;ss_res+=s.lr_resid[i]*s.lr_resid[i];
        const float d=s.lr_y[i]-my;ss_tot+=d*d;}
    s.lr_r2=ss_tot>1e-6f?std::clamp(1.0f-ss_res/ss_tot,0.0f,1.0f):1.0f;
}

static void init_nn(MLState& s) {
    using namespace manas::nn;
    s.nn_model=Sequential{};
    s.nn_model.add(Dense<ActivationReLU>(2,32));
    s.nn_model.add(Dense<ActivationReLU>(32,16));
    s.nn_model.add(Dense<ActivationSigmoid>(16,1));
    s.nn_opt=Adam<>{0.005f}; s.nn_loss_hist.clear(); s.nn_epoch=0; s.nn_init=true;
}

static void step_nn(MLState& s) {
    if (!s.nn_init) return;
    const ts::tensor<float> X({4,2},{0.f,0.f,0.f,1.f,1.f,0.f,1.f,1.f});
    const ts::tensor<float> Y({4,1},{0.f,1.f,1.f,0.f});
    for (int i=0;i<8;++i){
        s.nn_loss_cur=manas::nn::train_step(s.nn_model,manas::nn::TensorVar(X),manas::nn::TensorVar(Y),manas::nn::mse_loss,s.nn_opt);
        ++s.nn_epoch;}
    s.nn_loss_hist.push_back(s.nn_loss_cur);
    if (static_cast<int>(s.nn_loss_hist.size())>MLState::NN_HIST) s.nn_loss_hist.erase(s.nn_loss_hist.begin());
    const auto pt=manas::nn::predict(s.nn_model,manas::nn::TensorVar(X));
    const float ty[4]={0,1,1,0}; int c=0;
    for (int i=0;i<4;++i) if (std::abs(pt.data(std::vector<size_t>{size_t(i),size_t(0)})-ty[i])<0.5f) ++c;
    s.nn_acc=static_cast<float>(c)/4.0f;
}

static void update_solvers(MLState& s) {
    const double tx=50.0+40.0*std::sin(static_cast<double>(s.t)*0.35);
    const double ty=50.0+35.0*std::cos(static_cast<double>(s.t)*0.28);
    auto obj=[&](const ga::Vector<double>& x)->double{const double dx=x[0]-tx,dy=x[1]-ty;return dx*dx+dy*dy;};
    const auto prob=kalpa::make_problem(obj);
    ga::Vector<double> x0(2); x0[0]=50.0;x0[1]=50.0;
    auto run=[&](int idx,auto algo){
        using Algo=decltype(algo); using Deriv=kalpa::Derivatives<kalpa::FiniteDiff,double>;
        kalpa::Solver<Algo,Deriv> slv(std::move(algo)); const auto res=slv.solve(prob,x0);
        const float fv=res?static_cast<float>(res->f):(s.solver_hist[idx].empty()?1000.f:s.solver_hist[idx].back());
        s.solver_best[idx]=std::min(s.solver_best[idx],fv);
        if (res){s.solver_iters[idx]=res->iterations;}
        s.solver_hist[idx].push_back(fv);
        if (static_cast<int>(s.solver_hist[idx].size())>MLState::KALPA_HIST) s.solver_hist[idx].erase(s.solver_hist[idx].begin());
        s.solver_spark[idx].push_back(fv);
        if (static_cast<int>(s.solver_spark[idx].size())>MLState::SPARK) s.solver_spark[idx].erase(s.solver_spark[idx].begin());
    };
    run(0,kalpa::GradientDescent<double>{}); run(1,kalpa::Momentum<double>{});
    run(2,kalpa::Nesterov<double>{}); run(3,kalpa::Adam<double>{}); run(4,kalpa::LBFGS<double>{});
}


// ─────────────────────────────────────────────────────────────────────────────
// Figure builders
// ─────────────────────────────────────────────────────────────────────────────
static rekha::Figure make_kmeans_fig(const MLState& s) {
    const int N=MLState::KM_N,K=MLState::KM_K;
    rekha::Figure fig;
    fig.theme(rekha::Figure::theme_dark_neon())
       .axes(rekha::Axes{.x_label="x",.y_label="y",.ticks=4,
                         .x_range_override=true,.y_range_override=true,.x_range={0,100},.y_range={0,100}});
    for (int k=0;k<K;++k){
        rekha::XYSeries ser("c"+std::to_string(k));
        ser.stroke({pal::k_cluster[k],0.0f}).marker({pal::k_cluster[k],4.0f});
        for (int i=0;i<N;++i) if (s.km_labels[i]==k) ser.add(s.km_x[i],s.km_y[i]);
        fig.add(rekha::ScatterPlot{std::move(ser)});}
    for (int k=0;k<K;++k){
        rekha::XYSeries c("cen"+std::to_string(k));
        c.stroke({pal::k_cluster[k],2.0f}).marker({kalpana::colors::white(),9.0f});
        c.add(s.km_cx[k],s.km_cy[k]); fig.add(rekha::ScatterPlot{std::move(c)});}
    rekha::BubblePlot bp; bp.stroke={kalpana::Color{1,0.9f,0.3f,0.5f},0.8f};
    for (int k=0;k<K;++k){
        const float r=std::clamp(std::sqrt(s.km_inertia/N)*0.4f,2.0f,18.0f);
        bp.points.push_back({s.km_cx[k],s.km_cy[k],r,kalpana::Color{1,0.9f,0.3f,0.22f}});}
    fig.add(std::move(bp)); return fig;
}
static rekha::Figure make_regression_fig(const MLState& s) {
    const int N=MLState::LR_N;
    rekha::XYSeries obs("obs"); obs.stroke({pal::k_accent,0.0f}).marker({pal::k_accent,3.0f});
    for (int i=0;i<N;++i) obs.add(s.lr_x[i],s.lr_y[i]);
    rekha::XYSeries fit("fit"); fit.stroke({pal::k_orange,2.2f}).marker({pal::k_orange,0.0f});
    for (int i=0;i<static_cast<int>(s.lr_pred_x.size());++i) fit.add(s.lr_pred_x[i],s.lr_pred_y[i]);
    rekha::XYSeries res("res"); res.stroke({kalpana::Color{0.9f,0.3f,0.3f,0.4f},0.8f}).marker({});
    for (int i=0;i<N;++i){res.add(s.lr_x[i],s.lr_y[i]);res.add(s.lr_x[i],std::clamp(s.lr_y[i]-s.lr_resid[i],0.0f,100.0f));}
    rekha::Figure fig;
    fig.theme(rekha::Figure::theme_dark_neon())
       .axes(rekha::Axes{.x_label="x",.y_label="signal",.ticks=4,
                         .x_range_override=true,.y_range_override=true,.x_range={0,100},.y_range={0,100}})
       .add(rekha::LinePlot{std::move(res)}).add(rekha::ScatterPlot{std::move(obs)}).add(rekha::LinePlot{std::move(fit)});
    return fig;
}
static rekha::Figure make_nn_fig(const MLState& s) {
    const int H2=static_cast<int>(s.nn_loss_hist.size());
    float mx=1.0f; for (float v:s.nn_loss_hist) mx=std::max(mx,v); const float rng=mx+1e-5f;
    rekha::XYSeries loss("loss"); loss.stroke({pal::k_red,2.2f});
    for (int i=0;i<H2;++i) loss.add(static_cast<float>(s.nn_epoch-H2+i),s.nn_loss_hist[i]/rng*100.0f);
    rekha::XYSeries sm("smooth"); sm.stroke({pal::k_yellow,1.2f});
    for (int i=0;i<H2;++i){int lo=std::max(0,i-8);float avg=0;int cnt=0;
        for (int j=lo;j<=i;++j){avg+=s.nn_loss_hist[j];++cnt;}avg/=cnt;
        sm.add(static_cast<float>(s.nn_epoch-H2+i),avg/rng*100.0f);}
    rekha::XYSeries acc("acc"); acc.stroke({pal::k_green,1.0f});
    if (H2>0){acc.add(static_cast<float>(s.nn_epoch-H2),s.nn_acc*100.0f);acc.add(static_cast<float>(s.nn_epoch),s.nn_acc*100.0f);}
    rekha::XYSeries area("area"); area.stroke({pal::k_red,0.0f});
    for (int i=0;i<H2;++i) area.add(static_cast<float>(s.nn_epoch-H2+i),s.nn_loss_hist[i]/rng*100.0f);
    const float x0=H2>0?static_cast<float>(s.nn_epoch-H2):0.0f, x1=static_cast<float>(std::max(1,s.nn_epoch));
    rekha::Figure fig;
    fig.theme(rekha::Figure::theme_dark_neon())
       .axes(rekha::Axes{.x_label="epoch",.y_label="loss",.ticks=4,
                         .x_range_override=true,.y_range_override=true,.x_range={x0,x1},.y_range={0,100}})
       .add(rekha::AreaPlot{area,0.0f,0.12f}).add(rekha::LinePlot{std::move(loss)})
       .add(rekha::LinePlot{std::move(sm)}).add(rekha::LinePlot{std::move(acc)});
    return fig;
}
static rekha::Figure make_solver_fig(const MLState& s) {
    const char* sn[5]={"GD","Momentum","Nesterov","Adam","LBFGS"};
    float mx=0.1f; for (int si=0;si<5;++si) for (float v:s.solver_hist[si]) mx=std::max(mx,v);
    rekha::Figure fig;
    fig.theme(rekha::Figure::theme_dark_neon()).legend(true)
       .axes(rekha::Axes{.x_label="iter",.y_label="f(x)",.ticks=4,
                         .x_range_override=true,.y_range_override=true,
                         .x_range={0,static_cast<float>(MLState::KALPA_HIST)},.y_range={0,mx>1e-5f?mx:1.0f}});
    for (int si=0;si<5;++si){
        rekha::XYSeries ser(sn[si]); ser.stroke({pal::k_solver[si],1.8f});
        const int H2=static_cast<int>(s.solver_hist[si].size());
        const int off=std::max(0,MLState::KALPA_HIST-H2);
        for (int i=0;i<H2;++i) ser.add(static_cast<float>(off+i),s.solver_hist[si][i]);
        fig.add(rekha::LinePlot{std::move(ser)});}
    for (int si=0;si<5;++si){
        if (s.solver_hist[si].empty()) continue;
        rekha::XYSeries pt(sn[si]); pt.stroke({pal::k_solver[si],0.0f}).marker({pal::k_solver[si],7.0f});
        const int H2=static_cast<int>(s.solver_hist[si].size());
        pt.add(static_cast<float>(std::max(0,MLState::KALPA_HIST-H2)+H2-1),s.solver_hist[si].back());
        fig.add(rekha::ScatterPlot{std::move(pt)});}
    return fig;
}


// ─────────────────────────────────────────────────────────────────────────────
// Custom display widgets
// ─────────────────────────────────────────────────────────────────────────────
struct PanelHeader : pebble::drishya::widgets::WidgetBase {
    std::string title{};
    uint32_t    accent_color = pal::accent;
    PanelHeader() = default;
    PanelHeader(std::string t, uint32_t c) : title(std::move(t)), accent_color(c) {
        style_.height = akruti::layout::SizeSpec::Px(26.0f);
    }
    template<pebble::drishya::ITextMetrics M>
    [[nodiscard]] pebble::drishya::Size2D measure(const pebble::drishya::MeasureCtxT<M>&) const noexcept {
        return {static_cast<float>(title.size())*7.5f+20.0f,26.0f};
    }
    template<typename P> requires pebble::drishya::Painter<P>
    void paint(P& painter, pebble::drishya::Rect2D box) const {
        static int dbg = 0;
        if (dbg++ < 4) std::fprintf(stderr, "PanelHeader[%s] box=%.1f,%.1f,%.1f,%.1f\n",
            title.c_str(), box.x, box.y, box.w, box.h);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(accent_color);
        painter.fill_rect({box.x,box.y,3.0f,box.h});
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(pal::text_c);
        painter.text(std::string_view{title},{box.x+10.0f,box.y+box.h*0.55f+5.0f},11.0f);
    }
};

struct LiveBadge : pebble::drishya::widgets::WidgetBase {
    const MLState* ml = nullptr;
    explicit LiveBadge(const MLState* m) : ml(m) {
        style_.width = akruti::layout::SizeSpec::Px(80.0f);
        style_.height= akruti::layout::SizeSpec::Px(26.0f);
    }
    template<pebble::drishya::ITextMetrics M>
    [[nodiscard]] pebble::drishya::Size2D measure(const pebble::drishya::MeasureCtxT<M>&) const noexcept { return {80.0f,26.0f}; }
    template<typename P> requires pebble::drishya::Painter<P>
    void paint(P& painter, pebble::drishya::Rect2D box) const {
        if (!ml) return;
        const float p=0.5f+0.5f*std::sin(ml->t*5.0f);
        const uint32_t c=ml->paused?pal::orange:static_cast<uint32_t>(0xFF000000u|(uint32_t((0x10+int(0x10*p))&0xFF)<<16)|(0xB2u<<8)|uint32_t((0x59+int(0x10*p))&0xFF));
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(c);
        painter.round_rect(box,4.0f);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(0xFFFFFFFFu);
        painter.text(ml->paused?"PAUSED":"● LIVE",{box.x+8.0f,box.y+17.0f},10.0f);
    }
};

struct LiveStatTile : pebble::drishya::widgets::WidgetBase {
    const MLState* ml = nullptr;
    int            stat_id = 0; // 0=epoch,1=loss,2=acc,3=r2,4=inertia
    uint32_t       val_color = pal::accent;
    LiveStatTile() = default;
    LiveStatTile(const MLState* m, int sid, uint32_t vc) : ml(m), stat_id(sid), val_color(vc) {
        style_.height = akruti::layout::SizeSpec::Px(50.0f);
        style_.width  = akruti::layout::SizeSpec::Fr(1.0f);
        style_.margin = akruti::layout::Edges{0,0,0,4};
    }
    template<pebble::drishya::ITextMetrics M>
    [[nodiscard]] pebble::drishya::Size2D measure(const pebble::drishya::MeasureCtxT<M>&) const noexcept { return {80.0f,50.0f}; }
    template<typename P> requires pebble::drishya::Painter<P>
    void paint(P& painter, pebble::drishya::Rect2D box) const {
        if (!ml) return;
        static constexpr const char* captions[]  = {"Epoch","Loss","Accuracy","R²","Inertia"};
        static constexpr const char* fmts[]      = {"%.0f","%.5f","%.0f%%","%.4f","%.0f"};
        const float vals[]={static_cast<float>(ml->nn_epoch),ml->nn_loss_cur,ml->nn_acc*100.0f,ml->lr_r2,ml->km_inertia};
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(pal::dim);
        painter.text(std::string_view{captions[stat_id]},{box.x+8.0f,box.y+14.0f},9.0f);
        char buf[24]; std::snprintf(buf,sizeof(buf),fmts[stat_id],vals[stat_id]);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(val_color);
        painter.text(std::string_view{buf},{box.x+8.0f,box.y+34.0f},13.0f);
    }
};

struct LiveSparkline : pebble::drishya::widgets::WidgetBase {
    const std::vector<float>* data = nullptr;
    uint32_t color = pal::accent;
    LiveSparkline() = default;
    LiveSparkline(const std::vector<float>* d, uint32_t c) : data(d), color(c) {
        style_.flex_grow=1.0f; style_.width=akruti::layout::SizeSpec::Fr(1.0f);
        style_.height=akruti::layout::SizeSpec::Px(36.0f);
    }
    template<pebble::drishya::ITextMetrics M>
    [[nodiscard]] pebble::drishya::Size2D measure(const pebble::drishya::MeasureCtxT<M>&) const noexcept { return {80.0f,36.0f}; }
    template<typename P> requires pebble::drishya::Painter<P>
    void paint(P& painter, pebble::drishya::Rect2D box) const {
        if (!data || data->size()<2) return;
        float lo=(*data)[0],hi=(*data)[0];
        for (float v:*data){lo=v<lo?v:lo;hi=v>hi?v:hi;}
        const float span=hi-lo>0.0f?hi-lo:1.0f;
        const float step=box.w/static_cast<float>(data->size()-1);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(color);
        for (std::size_t i=1;i<data->size();++i){
            const float x0=box.x+step*static_cast<float>(i-1);
            const float x1=box.x+step*static_cast<float>(i);
            const float y0=box.y+box.h-((*data)[i-1]-lo)/span*box.h;
            const float y1=box.y+box.h-((*data)[i]-lo)/span*box.h;
            painter.line({x0,y0},{x1,y1},1.3f);}
    }
};

struct SolverBottomCell : pebble::drishya::widgets::WidgetBase {
    const MLState* ml  = nullptr;
    int            idx = 0;
    SolverBottomCell() = default;
    SolverBottomCell(const MLState* m, int i) : ml(m), idx(i) {
        style_.flex_grow=1.0f; style_.width=akruti::layout::SizeSpec::Fr(1.0f);
        style_.padding=akruti::layout::Edges{6,6,6,6};
    }
    template<pebble::drishya::ITextMetrics M>
    [[nodiscard]] pebble::drishya::Size2D measure(const pebble::drishya::MeasureCtxT<M>&) const noexcept { return {100.0f,90.0f}; }
    template<typename P> requires pebble::drishya::Painter<P>
    void paint(P& painter, pebble::drishya::Rect2D box) const {
        if (!ml) return;
        static constexpr const char* names[5]={"GD","Momentum","Nesterov","Adam","LBFGS"};
        const uint32_t col=pal::solver[idx];
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(pal::panel);
        painter.round_rect(box,4.0f);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(col);
        painter.text(std::string_view{names[idx]},{box.x+8.0f,box.y+16.0f},11.0f);
        char buf[24]; std::snprintf(buf,sizeof(buf),"f=%.2f",ml->solver_best[idx]>1e8f?0.0f:ml->solver_best[idx]);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(pal::dim);
        painter.text(std::string_view{buf},{box.x+8.0f,box.y+30.0f},9.0f);
        const auto& sp=ml->solver_spark[idx];
        if (sp.size()<2){return;}
        float lo=sp[0],hi=sp[0]; for (float v:sp){lo=v<lo?v:lo;hi=v>hi?v:hi;}
        const float span=hi-lo>0.0f?hi-lo:1.0f;
        const float sx=box.x+6, sw=box.w-12, sy=box.y+42, sh=box.h-50;
        const float step=sw/static_cast<float>(sp.size()-1);
        if constexpr (pebble::drishya::ColorPainter<P>) painter.set_color(col);
        for (std::size_t i=1;i<sp.size();++i){
            const float x0=sx+step*static_cast<float>(i-1);
            const float x1=sx+step*static_cast<float>(i);
            const float y0=sy+sh-(sp[i-1]-lo)/span*sh;
            const float y1=sy+sh-(sp[i]-lo)/span*sh;
            painter.line({x0,y0},{x1,y1},1.3f);}
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Thin figure widget — pointer-only, fits AnyWidget SBO.
// Rendering reuses RekhaWidget's translate logic via a static-local backend.
// ─────────────────────────────────────────────────────────────────────────────
struct LiveFigWidget : pebble::drishya::widgets::WidgetBase {
    const rekha::Figure* fig = nullptr;
    explicit LiveFigWidget(const rekha::Figure* f) : fig(f) {
        style_.flex_grow = 1.0f;
        style_.width = akruti::layout::SizeSpec::Fr(1.0f);
    }
    template<pebble::drishya::ITextMetrics M>
    [[nodiscard]] pebble::drishya::Size2D measure(const pebble::drishya::MeasureCtxT<M>&) const noexcept {
        return {200.0f, 150.0f};
    }
    template<typename P> requires pebble::drishya::Painter<P>
    void paint(P& painter, pebble::drishya::Rect2D box) const {
        static int dbg = 0;
        if (dbg++ < 8) std::fprintf(stderr, "LiveFigWidget paint fig=%p box=%.1f,%.1f,%.1f,%.1f\n",
            (void*)fig, box.x, box.y, box.w, box.h);
        if (!fig || box.w < 4 || box.h < 4) return;
        if constexpr (requires { painter.scene(); }) {
            const auto w = static_cast<std::uint32_t>(box.w);
            const auto h = static_cast<std::uint32_t>(box.h);
            static rekha::KalpanaBackend bk;
            rekha::Figure render_fig = *fig;
            render_fig.viewport({w, h, {}});
            render_fig.render(bk);
            bk.end_frame();
            kalpana::Node& root = const_cast<kalpana::Node&>(bk.scene().root());
            if (auto* grp = std::get_if<kalpana::GroupNode>(&root.content)) {
                for (kalpana::Node& ch : grp->children) {
                    translate_child(ch, box.x, box.y);
                    painter.scene().add(ch);
                }
            }
        }
    }
private:
    static void translate_child(kalpana::Node& n, float ox, float oy) {
        if (auto* g = std::get_if<kalpana::GroupNode>(&n.content))
            for (kalpana::Node& ch : g->children) translate_child(ch, ox, oy);
        else if (std::get_if<kalpana::ShapeNode>(&n.content))
            n.xf = kalpana::Transform::translate(ox, oy).combine(n.xf);
        else if (auto* t = std::get_if<kalpana::TextNode>(&n.content))
            { t->x += ox; t->y += oy; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// App wiring
// ─────────────────────────────────────────────────────────────────────────────
using Metrics    = pebble::drishya::MonospaceMetrics;
using SokolCanvas= kalpana::Canvas<kalpana::sokol_backend>;
using DPainter   = pebble::drishya::KalpanaPainter<SokolCanvas, Metrics>;
using DApp       = pebble::drishya::App<Metrics, DPainter>;

using namespace akruti::layout;

struct MLDashApp {
    sg_pipeline   pip{};
    sg_bindings   bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{}, ibuf{};
    std::unique_ptr<SokolCanvas> canvas;
    std::unique_ptr<Metrics>  metrics;
    std::unique_ptr<DApp>     ui;
    std::unique_ptr<DPainter> painter;
    MLState ml{};
    // Live figures — updated each frame; RekhaWidgets hold const* to these
    rekha::Figure fig_kmeans{};
    rekha::Figure fig_regression{};
    rekha::Figure fig_nn{};
    rekha::Figure fig_solver{};
};

MLDashApp g_app{};

static void build_ui(MLDashApp& app) {
    auto& ui  = *app.ui;
    auto& ml  = app.ml;
    ui.clear();

    // Root: full-window column
    const auto root = ui.set_root([]{
        pebble::drishya::widgets::Stack s=pebble::drishya::widgets::vstack();
        s.width(SizeSpec::Px(W)).height(SizeSpec::Px(H)).align(Align::Stretch);
        return s;
    }());

    // ── Title bar ─────────────────────────────────────────────────────────────
    const auto tbar = ui.add_child(root,[]{
        pebble::drishya::widgets::Panel p=pebble::drishya::widgets::panel();
        p.fill(pal::titlebar).axis(Axis::Row).pad(0.0f);
        p.style_.height=SizeSpec::Px(38.0f); p.style_.align_items=Align::Center;
        p.style_.padding=Edges{10,4,8,4}; return p;
    }());
    ui.add_child(tbar,[]{
        pebble::drishya::widgets::Label l("PEBBLE INTELLIGENCE DASHBOARD",pal::accent,14.0f);
        l.style_.flex_grow=1.0f; return l;
    }());
    ui.add_child(tbar, LiveBadge{&ml});

    // ── Content row ───────────────────────────────────────────────────────────
    const auto content = ui.add_child(root,[]{
        pebble::drishya::widgets::Stack s=pebble::drishya::widgets::hstack();
        s.grow(1.0f).align(Align::Stretch); return s;
    }());

    // ── Sidebar ───────────────────────────────────────────────────────────────
    const auto sidebar = ui.add_child(content,[]{
        pebble::drishya::widgets::Panel p=pebble::drishya::widgets::panel();
        p.fill(pal::sidebar).axis(Axis::Column).pad(8.0f);
        p.style_.width=SizeSpec::Px(178.0f); return p;
    }());
    ui.add_child(sidebar,pebble::drishya::widgets::Label{"PEBBLE AI",pal::accent,15.0f});
    ui.add_child(sidebar,pebble::drishya::widgets::Label{"ML · OPTIM · NEURAL",pal::dim,9.0f});
    ui.add_child(sidebar,pebble::drishya::widgets::strut(8.0f));

    const uint32_t tile_vc[5]={pal::accent,pal::red,pal::green,pal::purple,pal::teal};
    for (int i=0;i<5;++i) ui.add_child(sidebar, LiveStatTile{&ml,i,tile_vc[i]});
    ui.add_child(sidebar,pebble::drishya::widgets::strut(6.0f));
    ui.add_child(sidebar,pebble::drishya::widgets::Label{"NN Loss",pal::dim,9.0f});
    ui.add_child(sidebar, LiveSparkline{&ml.nn_loss_hist, pal::red});

    // ── Panels 2×2 ────────────────────────────────────────────────────────────
    const auto panels_col = ui.add_child(content,[]{
        pebble::drishya::widgets::Stack s=pebble::drishya::widgets::vstack(4.0f);
        s.grow(1.0f).align(Align::Stretch).padding(Edges{4,4,4,4}); return s;
    }());

    struct PanelDef { const char* title; uint32_t color; };
    const PanelDef pd[4]={
        {"K-Means Clustering  [120pts, 3 clusters]",pal::accent},
        {"Polynomial Regression  [cubic, live R²]",pal::orange},
        {"Neural Net XOR  [2→32→16→1, Adam]",pal::red},
        {"Kalpa Solvers  [GD/Mom/Nes/Adam/LBFGS]",pal::green},
    };

    for (int row=0;row<2;++row) {
        const auto r=ui.add_child(panels_col,[]{
            pebble::drishya::widgets::Stack s=pebble::drishya::widgets::hstack(0.0f);
            s.grow(1.0f).align(Align::Stretch); return s;
        }());
        for (int col=0;col<2;++col) {
            const int pi=row*2+col;
            const auto p=ui.add_child(r,[&]{
                pebble::drishya::widgets::Panel pan=pebble::drishya::widgets::panel();
                pan.fill(pal::panel).rounded(6.0f).axis(Axis::Column).pad(0.0f);
                pan.style_.flex_grow=1.0f; pan.style_.width=SizeSpec::Fr(1.0f);
                pan.style_.align_items=Align::Stretch;
                pan.style_.margin=Edges{0,col<1?4.0f:0.0f,0,0}; return pan;
            }());
            ui.add_child(p, PanelHeader{pd[pi].title, pd[pi].color});
            {
                const rekha::Figure* figs[4] = {&app.fig_kmeans,&app.fig_regression,&app.fig_nn,&app.fig_solver};
                ui.add_child(p, LiveFigWidget{figs[pi]});
            }
        }
    }

    // ── Bottom strip ──────────────────────────────────────────────────────────
    const auto bottom=ui.add_child(root,[]{
        pebble::drishya::widgets::Panel p=pebble::drishya::widgets::panel();
        p.fill(pal::titlebar).axis(Axis::Row).pad(4.0f);
        p.style_.height=SizeSpec::Px(90.0f); p.style_.align_items=Align::Stretch; return p;
    }());
    for (int si=0;si<5;++si)
        ui.add_child(bottom, SolverBottomCell{&ml,si});

    ui.set_viewport({0,0,static_cast<float>(W),static_cast<float>(H)});
    ui.solve();
    // debug: dump first 12 engine rects
    const auto& eng = ui.layout().engine();
    for (std::size_t i=0; i<std::min(eng.rect.size(), std::size_t(12)); ++i)
        std::fprintf(stderr, "node[%zu] axis=%d flex_grow=%.1f w_spec=%d h_spec=%d rect=%.0f,%.0f,%.0f,%.0f\n",
            i, (int)eng.axis[i], eng.flex_grow[i],
            (int)eng.width[i].kind, (int)eng.height[i].kind,
            eng.rect[i].x, eng.rect[i].y, eng.rect[i].w, eng.rect[i].h);
}

void init_cb() {
    auto& app=g_app;
    sg_desc gfx{}; gfx.environment=sglue_environment(); gfx.logger.func=slog_func; sg_setup(&gfx);
    {sg_buffer_desc d{}; d.size=768*1024*sizeof(kalpana::sokol_backend::Vertex);
     d.usage.stream_update=true; app.vbuf=sg_make_buffer(d); app.bind.vertex_buffers[0]=app.vbuf;}
    {sg_buffer_desc d{}; d.size=1536*1024*sizeof(uint32_t);
     d.usage.index_buffer=true; d.usage.stream_update=true; app.ibuf=sg_make_buffer(d); app.bind.index_buffer=app.ibuf;}
    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source=VS_METAL; shd.vertex_func.entry="vs";
    shd.fragment_func.source=FS_METAL; shd.fragment_func.entry="fs";
#else
    shd.vertex_func.source=VS_GLSL; shd.fragment_func.source=FS_GLSL;
#endif
    sg_shader shdr=sg_make_shader(shd);
    sg_pipeline_desc pd{}; pd.shader=shdr; pd.index_type=SG_INDEXTYPE_UINT32;
    pd.layout.attrs[0].format=SG_VERTEXFORMAT_FLOAT2; pd.layout.attrs[1].format=SG_VERTEXFORMAT_FLOAT4;
    app.pip=sg_make_pipeline(pd);
    app.pass_action.colors[0].load_action=SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value={0.03f,0.03f,0.07f,1.0f};
    app.canvas  =std::make_unique<SokolCanvas>(W,H);
    app.metrics =std::make_unique<Metrics>();
    app.ui      =std::make_unique<DApp>(*app.metrics);
    app.painter =std::make_unique<DPainter>(*app.canvas,*app.metrics);
    auto& s=app.ml;
    update_kmeans(s); update_regression(s); init_nn(s);
    app.fig_kmeans    = make_kmeans_fig(s);
    app.fig_regression= make_regression_fig(s);
    app.fig_nn        = make_nn_fig(s);
    app.fig_solver    = make_solver_fig(s);
    build_ui(app);
}

void frame_cb() {
    auto& app=g_app; auto& s=app.ml; auto& ui=*app.ui;
    if (!s.paused){
        s.t+=DT; ++s.frame;
        if (s.frame%2==0) update_kmeans(s);
        if (s.frame%3==0) update_regression(s);
        step_nn(s); update_solvers(s);
    }
    app.fig_kmeans    = make_kmeans_fig(s);
    app.fig_regression= make_regression_fig(s);
    app.fig_nn        = make_nn_fig(s);
    app.fig_solver    = make_solver_fig(s);
    app.painter->begin_frame();
    app.painter->set_clear_color(pal::bg);
    ui.paint(*app.painter);
    app.painter->present();
    const auto& verts=app.canvas->backend().vertices();
    const auto& indices=app.canvas->backend().indices();
    if (!verts.empty()&&!indices.empty()){
        sg_update_buffer(app.vbuf,sg_range{verts.data(),verts.size()*sizeof(kalpana::sokol_backend::Vertex)});
        sg_update_buffer(app.ibuf,sg_range{indices.data(),indices.size()*sizeof(uint32_t)});}
    sg_pass pass{}; pass.action=app.pass_action; pass.swapchain=sglue_swapchain();
    sg_begin_pass(pass);
    if (!indices.empty()){sg_apply_pipeline(app.pip);sg_apply_bindings(app.bind);sg_draw(0,static_cast<int>(indices.size()),1);}
    sg_end_pass(); sg_commit();
}

void event_cb(const sapp_event* ev) {
    if (ev->type!=SAPP_EVENTTYPE_KEY_DOWN) return;
    switch (ev->key_code){
        case SAPP_KEYCODE_ESCAPE: sapp_quit(); break;
        case SAPP_KEYCODE_SPACE: g_app.ml.paused^=true; break;
        case SAPP_KEYCODE_R:
            g_app.ml.t=0;g_app.ml.frame=0;init_nn(g_app.ml);
            for (int i=0;i<MLState::NUM_SOLVERS;++i){g_app.ml.solver_hist[i].clear();g_app.ml.solver_spark[i].clear();g_app.ml.solver_best[i]=1e9f;}
            break;
        default: break;
    }
}
void cleanup_cb(){sg_shutdown();}

} // namespace

sapp_desc sokol_main(int,char**){
    sapp_desc d{};
    d.init_cb=init_cb; d.frame_cb=frame_cb; d.event_cb=event_cb; d.cleanup_cb=cleanup_cb;
    d.width=W; d.height=H;
    d.window_title="Pebble Intelligence Dashboard  [SPACE pause | R reset | ESC quit]";
    d.icon.sokol_default=true; d.logger.func=slog_func;
    return d;
}
