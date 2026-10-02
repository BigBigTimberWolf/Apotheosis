#include "macro/rule_sources.h"
#include <opencv2/opencv.hpp>
#include <thread>
#include <cstdio>
#include <fstream>
#include <filesystem>
using namespace macros;
int64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main(){int failures=0;auto check=[&](bool value,const char* why){if(!value){++failures;std::printf("FAIL %s\n",why);}};
    cv::Mat frame(120,160,CV_8UC3,cv::Scalar(20,30,40));cv::rectangle(frame,{30,40,25,20},cv::Scalar(0,0,255),-1);cv::line(frame,{33,42},{50,56},cv::Scalar(255,255,255),2);
    cv::Mat pattern=frame(cv::Rect(30,40,25,20)).clone();std::vector<uchar> png;cv::imencode(".png",pattern,png);
    const std::string path=u8"宏模板测试.png";{std::ofstream out(std::filesystem::u8path(path),std::ios::binary);out.write(reinterpret_cast<const char*>(png.data()),png.size());}
    Condition hit;hit.metric="image.hit";hit.text=path;hit.upper=.95;
    Condition count;count.metric="pixels.count";count.text="ff0000";count.upper=0;
    Condition multi;multi.metric="pixels.multi";multi.text="31,41,ff0000;0,0,281e14";multi.upper=0;
    Condition pixel;pixel.metric="pixel.value";pixel.region={31,41,1,1};
    Condition missing=hit;missing.text="missing-template-file.png";
    Program p;p.enabled=true;p.conditions={hit,count,multi,pixel,missing};p.options["vision_interval_ms"]="50";configureSources({p},true);
    auto acquire=[&](int64_t captured){submitVisionFrame(frame,captured*1000000);RuleSnapshot s;for(int i=0;i<100;++i){s.now=now();s.values.clear();appendSources(s);if(s.values.count(sensorKey(hit)))break;std::this_thread::sleep_for(std::chrono::milliseconds(20));}return s;};
    auto s=acquire(now());check(metric(hit,s).known&&metric(hit,s).number==1,"unicode template path and perfect match");
    check(metric(count,s).known&&metric(count,s).number>300,"color count");check(metric(multi,s).known&&metric(multi,s).number==1,"multiple color points");
    check(metric(pixel,s).number==0xff0000,"ROI pixel RGB");check(!metric(missing,s).known,"missing template is unknown");
    Condition ocr;ocr.metric="ocr.text";ocr.text="123";ocr.comparison="contains";Program textRule;textRule.enabled=true;textRule.conditions={ocr};configureSources({textRule},true);
    cv::Mat textImage(160,480,CV_8UC3,cv::Scalar::all(255));cv::putText(textImage,"TEST 123",{20,110},cv::FONT_HERSHEY_SIMPLEX,2.4,cv::Scalar::all(0),4,cv::LINE_AA);
    submitVisionFrame(textImage,now()*1000000);Value recognized;
    for(int i=0;i<200;++i){RuleSnapshot textSnapshot;textSnapshot.now=now();appendSources(textSnapshot);auto it=textSnapshot.values.find(sensorKey(ocr));if(it!=textSnapshot.values.end()){recognized=it->second;break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    if(recognized.known){std::printf("OCR recognized: %s\n",recognized.text.c_str());check(recognized.text.find("TEST")!=std::string::npos,"OCR synthetic text");}
    else {std::printf("OCR environment unavailable: %s\n",recognized.error.c_str());check(!recognized.error.empty(),"OCR failure has diagnostic");}
    // Old capture time cannot become a fresh result just because processing completed now.
    configureSources({p},true);std::this_thread::sleep_for(std::chrono::milliseconds(60));submitVisionFrame(frame,(now()-5000)*1000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));s={};s.now=now();appendSources(s);check(!metric(hit,s).known,"stale captured frame rejected");
    stopSources();std::filesystem::remove(std::filesystem::u8path(path));std::printf("macro sources: %d failures\n",failures);return failures?1:0;
}
