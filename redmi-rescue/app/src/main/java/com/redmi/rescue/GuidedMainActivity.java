package com.redmi.rescue;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.os.StatFs;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Locale;

public final class GuidedMainActivity extends Activity {
    private static final int BG=Color.rgb(9,19,27);
    private static final int PANEL=Color.rgb(21,38,47);
    private static final int INK=Color.rgb(236,247,242);
    private static final int MUTE=Color.rgb(155,180,183);
    private static final int ACCENT=Color.rgb(120,233,186);
    private LinearLayout base,card;
    private TextView step,title,explain,summary,status,alternative;
    private Button primary;
    private TextView workerDisplay;
    private String folderSummary="",appFull="";
    private int phase=0; // 0=preparation, 1=scan, 2=complete
    private boolean pausedForSettings=false;
    private static final String NL="\n";

    @Override public void onCreate(Bundle b){
      super.onCreate(b);
      getWindow().setStatusBarColor(BG);
      getWindow().setNavigationBarColor(BG);
      ScrollView scroll=new ScrollView(this);
      scroll.setFillViewport(true);
      base=new LinearLayout(this);base.setOrientation(LinearLayout.VERTICAL);
      base.setPadding(dp(22),dp(24),dp(22),dp(24));base.setBackgroundColor(BG);
      scroll.addView(base);
      setContentView(scroll);

      TextView brand=label("●  REDMI RESCUE",14,ACCENT,true);
      brand.setLetterSpacing(.10f);
      TextView tagline=label("Encontre o que ocupa espaço. Sem apagar nada.",13,MUTE,false);
      tagline.setPadding(0,dp(2),0,dp(26));

      card=new LinearLayout(this);card.setOrientation(LinearLayout.VERTICAL);
      card.setPadding(dp(18),dp(20),dp(18),dp(20));
      card.setBackground(background(PANEL,18));
      base.addView(card,new LinearLayout.LayoutParams(-1,-2));

      step=inside("ETAPA 1 DE 3",12,ACCENT,true);
      title=inside("Preparar o diagnóstico",23,INK,true);
      title.setPadding(0,dp(8),0,dp(12));
      explain=inside("",14,MUTE,false);
      explain.setLineSpacing(dp(3),1f);
      status=inside("",14,INK,false);
      status.setPadding(0,dp(18),0,dp(14));
      primary=new Button(this);
      primary.setTextColor(BG);primary.setTextSize(15);
      primary.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
      primary.setAllCaps(false);primary.setMinHeight(dp(52));
      primary.setBackground(background(ACCENT,13));
      primary.setOnClickListener(v->proceed());
      LinearLayout.LayoutParams bp=new LinearLayout.LayoutParams(-1,dp(54));
      bp.topMargin=dp(8);card.addView(primary,bp);
      alternative=inside("",13,ACCENT,false);
      alternative.setGravity(Gravity.CENTER);
      alternative.setPadding(0,dp(16),0,dp(4));
      alternative.setVisibility(View.GONE);
      alternative.setOnClickListener(v->ReportVault.promptSave(this,true));

      TextView note=label("PRIVACIDADE",12,ACCENT,true);
      note.setPadding(0,dp(26),0,dp(8));
      TextView privacy=label("Nenhuma foto, conversa ou arquivo será excluído. O app consulta metadados, estatísticas agregadas e tamanhos permitidos pelo Android. Os relatórios ficam no aparelho até você decidir salvá-los ou compartilhá-los.",13,MUTE,false);
      privacy.setLineSpacing(dp(3),1f);
      workerDisplay=new TextView(this); // Output from background auditing modules, never a separate on-screen button.
      storeBaseline();
      showStep();
    }
    private int dp(float x){return (int)(x*getResources().getDisplayMetrics().density+.5f);}
    private GradientDrawable background(int color,int radius) {
      GradientDrawable bg=new GradientDrawable();
      bg.setColor(color);bg.setCornerRadius(dp(radius));return bg;
    }
    private TextView label(String text,int size,int color,boolean strong){
      TextView v=new TextView(this);v.setText(text);v.setTextSize(size);
      v.setTextColor(color);if(strong)v.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
      base.addView(v);return v;
    }
    private TextView inside(String text,int size,int color,boolean strong){
      TextView v=new TextView(this);v.setText(text);v.setTextSize(size);
      v.setTextColor(color);if(strong)v.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
      card.addView(v);return v;
    }
    private void storeBaseline(){
      StatFs fs=new StatFs(Environment.getDataDirectory().getAbsolutePath());
      ActivityManager m=(ActivityManager)getSystemService(Context.ACTIVITY_SERVICE);
      ActivityManager.MemoryInfo mi=new ActivityManager.MemoryInfo();
      m.getMemoryInfo(mi);
      Intent battery=registerReceiver(null,new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
      int t=battery==null?-1:battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,-1);
      PowerManager pm=(PowerManager)getSystemService(Context.POWER_SERVICE);
      int thermal=pm==null?-1:pm.getCurrentThermalStatus();
      String report="Dispositivo: "+Build.MANUFACTURER+" "+Build.MODEL+
        NL+"Android: "+Build.VERSION.RELEASE+
        NL+"Espaço livre: "+gb(fs.getAvailableBytes())+" / "+gb(fs.getTotalBytes())+
        NL+"RAM livre instantânea: "+gb(mi.availMem)+" / "+gb(mi.totalMem)+
        NL+"Temperatura da bateria: "+(t<0?"indisponível":String.format(Locale.forLanguageTag("pt-BR"),"%.1f °C",t/10.0))+
        NL+"Estado térmico: "+thermal+
        NL+"A temperatura da bateria não é a temperatura da CPU. RAM livre não mede velocidade.";
      ReportVault.setDiagnosis(report);
    }
    private String gb(long bytes){return String.format(Locale.forLanguageTag("pt-BR"),"%.2f GB",bytes/1073741824.0);}
    private boolean fileAccess(){return Build.VERSION.SDK_INT<30||Environment.isExternalStorageManager();}
    @Override protected void onResume(){
      super.onResume();
      if(phase==0 && primary!=null)showStep();
    }
    private void showStep(){
      if(phase==1){return;}
      if(phase==2){
        step.setText("ETAPA 3 DE 3  •  CONCLUÍDO");
        title.setText("Seu diagnóstico está pronto");
        explain.setText("Salve o TXT completo para investigar o consumo de espaço. Você também pode exportar uma tabela CSV para o Excel.");
        primary.setText("Salvar relatório TXT");
        primary.setEnabled(true);primary.setAlpha(1f);
        alternative.setText("Ou salvar tabela CSV (Excel)");
        alternative.setVisibility(View.VISIBLE);
        return;
      }
      step.setText("ETAPA 1 DE 3  •  AUTORIZAÇÕES");
      title.setText("Preparar a varredura");
      alternative.setVisibility(View.GONE);
      primary.setEnabled(true);primary.setAlpha(1f);
      if(!AppStorageAudit.hasPermission(this)){
        explain.setText("Primeiro, permita acesso às estatísticas de uso. Isso ajuda a identificar aplicativos com muitos dados, sem abrir o conteúdo deles.");
        status.setText("1/2  Acesso ao uso: pendente\n2/2  Arquivos: "+(fileAccess()?"autorizado":"pendente"));
        primary.setText("Autorizar acesso ao uso");
      } else if(!fileAccess()){
        explain.setText("Agora permita acesso aos arquivos compartilhados. O Redmi Rescue usa essa autorização para medir pastas e downloads, não para excluir seus arquivos.");
        status.setText("1/2  Acesso ao uso: autorizado\n2/2  Arquivos: pendente");
        primary.setText("Autorizar leitura de arquivos");
      } else {
        explain.setText("Tudo pronto. Vamos combinar uma varredura das pastas acessíveis com estatísticas de aplicativos e categorias que o Android permitir consultar.");
        status.setText("✓  Acesso ao uso\n✓  Leitura de arquivos\n\nNenhuma exclusão será executada.");
        primary.setText("Iniciar varredura");
      }
    }
    private void proceed(){
      if(phase==2){ReportVault.promptSave(this,false);return;}
      if(phase!=0)return;
      if(!AppStorageAudit.hasPermission(this)){pausedForSettings=true;AppStorageAudit.requestPermission(this);return;}
      if(!fileAccess()){pausedForSettings=true;StorageAudit.askPermission(this);return;}
      begin();
    }
    private void begin(){
      phase=1;storeBaseline();
      step.setText("ETAPA 2 DE 3  •  ANALISANDO");
      title.setText("Analisando o Redmi");
      explain.setText("Verificando pastas acessíveis e depois consultando estatísticas de aplicativos. Isso pode levar algum tempo. Não feche o aplicativo.");
      status.setText("1/2  Mapeando pastas e arquivos...");
      primary.setText("Varredura em andamento...");
      primary.setEnabled(false);primary.setAlpha(.60f);
      alternative.setVisibility(View.GONE);
      StorageAudit.start(this,workerDisplay,summary->{
        folderSummary=summary;
        status.setText("✓  Pastas mapeadas\n2/2  Consultando os dados dos aplicativos...");
        AppStorageAudit.start(this,workerDisplay,()->{
          appFull=workerDisplay.getText().toString();
          finishAudit();
        });
      });
    }
    private void finishAudit(){
      phase=2;
      // Include short, useful facts without producing a wall of technical text.
      StringBuilder report=new StringBuilder();
      report.append("RESULTADOS\n\n");
      if(!folderSummary.isEmpty()) {
        String[] lines=folderSummary.split("\n");
        for(int i=0;i<Math.min(11,lines.length);i++)report.append(lines[i]).append("\n");
      }
      report.append("\nDADOS DOS APLICATIVOS\n");
      if(!appFull.isEmpty()){
        String[] lines=appFull.split("\n");
        int shown=0;
        for(String line:lines) {
          if(line.startsWith("Dados de usuário/aplicativos:")||
             line.startsWith("Total compartilhado reportado:")||
             line.startsWith("Apps identificados:")||
             line.startsWith("Aplicativos encontrados:")||
             line.matches("\\d+\\. .*")) {
            report.append(line).append("\n");
            if(++shown>=8)break;
          }
        }
        if(shown==0)report.append("O Android não forneceu tamanhos individuais; consulte os detalhes no TXT.\n");
      }
      report.append("\nAs categorias podem se sobrepor e não devem ser somadas. Dados não identificados NÃO são lixo.");
      status.setText(report.toString());
      showStep();
    }
    @Override protected void onActivityResult(int requestCode,int resultCode,Intent data) {
      super.onActivityResult(requestCode,resultCode,data);
      if(requestCode==ReportVault.SAVE_TXT||requestCode==ReportVault.SAVE_CSV){
        if(resultCode!=RESULT_OK||data==null||data.getData()==null)return;
        boolean ok=ReportVault.write(this,data.getData(),requestCode==ReportVault.SAVE_CSV);
        new AlertDialog.Builder(this).setTitle(ok?"Arquivo salvo":"Falha ao salvar")
          .setMessage(ok?"Relatório salvo no local escolhido. Você pode anexar esse arquivo aqui para análise.":"Não consegui escrever o arquivo. Escolha outra pasta com espaço disponível.")
          .setPositiveButton("OK",null).show();
      }
    }
}
