/* 
 * Copyright (c) 2006 Sean C. Rhea (srhea@srhea.net)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "AllPlot.h"
#include "RideFile.h"
#include "RideItem.h"
#include "Settings.h"
#include "Zones.h"

#include <assert.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_marker.h>
#include <qwt_text.h>
#include <qwt_legend.h>
#include <qwt_data.h>
#include <QMultiMap>


// define a background class to handle shading of power zones
// draws power zone bands IF zones are defined and the option
// to draw bonds has been selected
class AllPlotBackground: public QwtPlotItem
{
    private:
        AllPlot *parent;

    public:
        AllPlotBackground(AllPlot *_parent)
        {
	    setZ(0.0);
	    parent = _parent;
	}

    virtual int rtti() const
    {
        return QwtPlotItem::Rtti_PlotUserItem;
    }

    virtual void draw(QPainter *painter,
		      const QwtScaleMap &, const QwtScaleMap &yMap,
		      const QRect &rect) const
    {
	RideItem *rideItem = parent->rideItem;

	if (! rideItem)
	    return;

	Zones **zones      = rideItem->zones;
	int zone_range     = rideItem->zoneRange();

	if (parent->shadeZones() && zones && *zones && (zone_range >= 0)) {
	    QList <int> zone_lows = (*zones)->getZoneLows(zone_range);
	    int num_zones = zone_lows.size();
	    if (num_zones > 0) {
		for (int z = 0; z < num_zones; z ++) {
		    QRect r = rect;

		    QColor shading_color = zoneColor(z, num_zones);
		    shading_color.setHsv(
					 shading_color.hue(),
					 shading_color.saturation() / 4,
					 shading_color.value()
					 );
		    r.setBottom(yMap.transform(zone_lows[z]));
		    if (z + 1 < num_zones)
			r.setTop(yMap.transform(zone_lows[z + 1]));
		    if (r.top() <= r.bottom())
			painter->fillRect(r, shading_color);
		}
	    }
	}
    }
};

// Zone labels are drawn if power zone bands are enabled, automatically
// at the center of the plot
class AllPlotZoneLabel: public QwtPlotItem
{
    private:
        AllPlot *parent;
        int zone_number;
        double watts;
        QwtText text;

    public:
        AllPlotZoneLabel(AllPlot *_parent, int _zone_number)
        {
	    parent = _parent;
	    zone_number = _zone_number;

	    RideItem *rideItem = parent->rideItem;


	    if (! rideItem)
		return;
	

	    Zones **zones      = rideItem->zones;
	    int zone_range     = rideItem->zoneRange();

	    // create new zone labels if we're shading
	    if (parent->shadeZones() && zones && *zones && (zone_range >= 0)) {
		QList <int> zone_lows = (*zones)->getZoneLows(zone_range);
		QList <QString> zone_names = (*zones)->getZoneNames(zone_range);
		int num_zones = zone_lows.size();
		assert(zone_names.size() == num_zones);
		if (zone_number < num_zones) {
		    watts =
			(
			 (zone_number + 1 < num_zones) ?
			 0.5 * (zone_lows[zone_number] + zone_lows[zone_number + 1]) :
			 (
			  (zone_number > 0) ?
			  (1.5 * zone_lows[zone_number] - 0.5 * zone_lows[zone_number - 1]) :
			  2.0 * zone_lows[zone_number]
			  )
			 );

		    text = QwtText(zone_names[zone_number]);
		    text.setFont(QFont("Helvetica",24, QFont::Bold));
		    QColor text_color = zoneColor(zone_number, num_zones);
		    text_color.setAlpha(64);
		    text.setColor(text_color);
		}
	    }

	    setZ(1.0 + zone_number / 100.0);
	}
    virtual int rtti() const
    {
	return QwtPlotItem::Rtti_PlotUserItem;
    }

    void draw(QPainter *painter,
				const QwtScaleMap &, const QwtScaleMap &yMap,
				const QRect &rect) const
    {
	if (parent->shadeZones()) {
	    int x = (rect.left() + rect.right()) / 2;
	    int y = yMap.transform(watts);

	    // the following code based on source for QwtPlotMarker::draw()
	    QRect tr(QPoint(0, 0), text.textSize(painter->font()));
	    tr.moveCenter(QPoint(x, y));
	    text.draw(painter, tr);
	}
    }
};


static inline double
max(double a, double b) { if (a > b) return a; else return b; }

#define MILES_PER_KM 0.62137119
#define FEET_PER_M 3.2808399

AllPlot::AllPlot():
    settings(NULL),
    unit(0),
    rideItem(NULL),
    smooth(30), bydist(false),
    shade_zones(false)
{
    boost::shared_ptr<QSettings> settings = GetApplicationSettings();    
    unit = settings->value(GC_UNIT);
    
    useMetricUnits = (unit.toString() == "Metric");

    // create a background object for shading
    bg = new AllPlotBackground(this);
    bg->attach(this);

    insertLegend(new QwtLegend(), QwtPlot::BottomLegend);
    setCanvasBackground(Qt::white);

    setXTitle();

    wattsCurve = new QwtPlotCurve("Power");
    QPen wattsPen = QPen(Qt::red);
    wattsPen.setWidth(2);
    wattsCurve->setPen(wattsPen);

    hrCurve = new QwtPlotCurve("Heart Rate");
    QPen hrPen = QPen(Qt::blue);
    hrPen.setWidth(2);
    hrCurve->setPen(hrPen);

    speedCurve = new QwtPlotCurve("Speed");
    QPen speedPen = QPen(QColor(0, 204, 0));
    speedPen.setWidth(2);
    speedCurve->setPen(speedPen);
    speedCurve->setYAxis(yRight);

    cadCurve = new QwtPlotCurve("Cadence");
    QPen cadPen = QPen(QColor(0, 204, 204));
    cadPen.setWidth(2);
    cadCurve->setPen(cadPen);

    altCurve = new QwtPlotCurve("Altitude");
    // altCurve->setRenderHint(QwtPlotItem::RenderAntialiased);
    QPen altPen(QColor(124, 91, 31));
    altPen.setWidth(1);
    altCurve->setPen(altPen);
    QColor brush_color = QColor(124, 91, 31);
    brush_color.setAlpha(64);
    altCurve->setBrush(brush_color);   // fill below the line

    grid = new QwtPlotGrid();
    grid->enableX(false);
    QPen gridPen;
    gridPen.setStyle(Qt::DotLine);
    grid->setPen(gridPen);
    grid->attach(this);

    zoneLabels = QList <AllPlotZoneLabel *>::QList();
}

struct DataPoint {
    double time, hr, watts, speed, cad, alt;
    int inter;
    DataPoint(double t, double h, double w, double s, double c, double a, int i) : 
        time(t), hr(h), watts(w), speed(s), cad(c), alt(a), inter(i) {}
};

bool AllPlot::shadeZones() const
{
    return (shade_zones && !wattsArray.empty());
}

void AllPlot::refreshZoneLabels()
{
    foreach(AllPlotZoneLabel *label, zoneLabels) {
        label->detach();
        delete label;
    }
    zoneLabels.clear();

    if (rideItem) {
	int zone_range = rideItem->zoneRange();
	Zones **zones  = rideItem->zones;

	// generate labels for existing zones
	if (zones && *zones && (zone_range >= 0)) {
	int num_zones = (*zones)->numZones(zone_range);
	for (int z = 0; z < num_zones; z ++) {
	    AllPlotZoneLabel *label = new AllPlotZoneLabel(this, z);
	    label->attach(this);
	    zoneLabels.append(label);
	}
	}
    }
}


void
AllPlot::recalc()
{
    if (timeArray.empty())
        return;
    int rideTimeSecs = (int) ceil(timeArray[arrayLength - 1]);
    if (rideTimeSecs > 7*24*60*60) {
        QwtArray<double> data;
	if (!wattsArray.empty())
	    wattsCurve->setData(data, data);
	if (!hrArray.empty())
	    hrCurve->setData(data, data);
	if (!speedArray.empty())
	    speedCurve->setData(data, data);
	if (!cadArray.empty())
	    cadCurve->setData(data, data);
	if (!altArray.empty())
	    altCurve->setData(data, data);
        return;
    }
    double totalWatts = 0.0;
    double totalHr = 0.0;
    double totalSpeed = 0.0;
    double totalCad = 0.0;
    double totalDist = 0.0;
    double totalAlt = 0.0;

    QList<DataPoint> list;

    QVector<double> smoothWatts(rideTimeSecs + 1);
    QVector<double> smoothHr(rideTimeSecs + 1);
    QVector<double> smoothSpeed(rideTimeSecs + 1);
    QVector<double> smoothCad(rideTimeSecs + 1);
    QVector<double> smoothTime(rideTimeSecs + 1);
    QVector<double> smoothDistance(rideTimeSecs + 1);
    QVector<double> smoothAltitude(rideTimeSecs + 1);

    QMap<double, int> interList; //Store the times and intervals
                             // Times are unique, intervals are not always
                             //Intervals are sequential on the PowerTap.

    int lastInterval = 0; //Detect if we hit a new interval

    for (int secs = 0; ((secs < smooth) 
                        && (secs < rideTimeSecs)); ++secs) {
        smoothWatts[secs] = 0.0;
        smoothHr[secs]    = 0.0;
        smoothSpeed[secs] = 0.0;
        smoothCad[secs]   = 0.0;
        smoothTime[secs]  = secs / 60.0;
        smoothDistance[secs]  = 0.0;
        smoothAltitude[secs]  = 0.0;
    }

    int i = 0;
    for (int secs = smooth; secs <= rideTimeSecs; ++secs) {
        while ((i < arrayLength) && (timeArray[i] <= secs)) {
            DataPoint dp(timeArray[i],
			 (!hrArray.empty() ? hrArray[i] : 0),
			 (!wattsArray.empty() ? wattsArray[i] : 0),
                         (!speedArray.empty() ? speedArray[i] : 0),
			 (!cadArray.empty() ? cadArray[i] : 0),
			 (!altArray.empty() ? altArray[i] : 0),
			 interArray[i]);
            if (!wattsArray.empty())
                totalWatts += wattsArray[i];
	    if (!hrArray.empty())
		totalHr    += hrArray[i];
	    if (!speedArray.empty())
		totalSpeed += speedArray[i];
	    if (!cadArray.empty())
		totalCad   += cadArray[i];
	    if (!altArray.empty())
		totalAlt   += altArray[i];
            totalDist   = distanceArray[i];
            list.append(dp);
            //Figure out when and if we have a new interval..
            if(lastInterval != interArray[i]) {
                lastInterval = interArray[i];
                interList[secs/60.0] = lastInterval;
            }
            ++i;
        }
        while (!list.empty() && (list.front().time < secs - smooth)) {
            DataPoint &dp = list.front();
            totalWatts -= dp.watts;
            totalHr    -= dp.hr;
            totalSpeed -= dp.speed;
            totalCad   -= dp.cad;
            totalAlt   -= dp.alt;
            list.removeFirst();
        }
        // TODO: this is wrong.  We should do a weighted average over the
        // seconds represented by each point...
        if (list.empty()) {
            smoothWatts[secs] = 0.0;
            smoothHr[secs]    = 0.0;
            smoothSpeed[secs] = 0.0;
            smoothCad[secs]   = 0.0;
            smoothAltitude[secs]   = smoothAltitude[secs - 1];
        }
        else {
            smoothWatts[secs]    = totalWatts / list.size();
            smoothHr[secs]       = totalHr / list.size();
            smoothSpeed[secs]    = totalSpeed / list.size();
            smoothCad[secs]      = totalCad / list.size();
	    smoothAltitude[secs]      = totalAlt / list.size();
        }
        smoothDistance[secs] = totalDist;
        smoothTime[secs]  = secs / 60.0;
    }

    QVector<double> &xaxis = bydist ? smoothDistance : smoothTime;

    // set curves
    if (!wattsArray.empty())
	wattsCurve->setData(xaxis.data(), smoothWatts.data(), rideTimeSecs + 1);
    if (!hrArray.empty())
	hrCurve->setData(xaxis.data(), smoothHr.data(), rideTimeSecs + 1);
    if (!speedArray.empty())
        speedCurve->setData(xaxis.data(), smoothSpeed.data(), rideTimeSecs + 1);
    if (!cadArray.empty())
        cadCurve->setData(xaxis.data(), smoothCad.data(), rideTimeSecs + 1);
    if (!altArray.empty())
        altCurve->setData(xaxis.data(), smoothAltitude.data(), rideTimeSecs + 1);

    setAxisScale(xBottom, 0.0, bydist ? totalDist : smoothTime[rideTimeSecs]);
    setYMax();

    refreshZoneLabels();

    foreach(QwtPlotMarker *mrk, d_mrk) {
        mrk->detach();
        delete mrk;
    }
    d_mrk.clear();
    foreach(double time, interList.keys()) {
        QwtPlotMarker *mrk = new QwtPlotMarker;
        d_mrk.append(mrk);
        mrk->attach(this);
        mrk->setValue(0,0);
        mrk->setLineStyle(QwtPlotMarker::VLine);
        mrk->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
        mrk->setLinePen(QPen(Qt::black, 0, Qt::DashDotLine));
        QwtText text(QString::number(interList[time]));
        text.setFont(QFont("Helvetica", 10, QFont::Bold));
        text.setColor(Qt::black);
        if (!bydist)
            mrk->setValue(time, 0.0);
        else
            mrk->setValue(smoothDistance[int(ceil(60*time))], 0.0);
        mrk->setLabel(text);
    }

    replot();
}

void
AllPlot::setYMax() 
{
    double ymax = 0;
    QString ylabel = "";
    if (wattsCurve->isVisible()) {
        ymax = max(ymax, wattsCurve->maxYValue());
        ylabel += QString((ylabel == "") ? "" : " / ") + "Watts";
    }
    if (hrCurve->isVisible()) {
        ymax = max(ymax, hrCurve->maxYValue());
        ylabel += QString((ylabel == "") ? "" : " / ") + "BPM";
    }
    if (cadCurve->isVisible()) {
        ymax = max(ymax, cadCurve->maxYValue());
        ylabel += QString((ylabel == "") ? "" : " / ") + "RPM";
    }
    if (altCurve->isVisible()) {
        ymax = max(ymax, altCurve->maxYValue());
	if (useMetricUnits){
        	ylabel += QString((ylabel == "") ? "" : " / ") + "Meters";
	} else {
		ylabel += QString((ylabel == "") ? "" : " / ") + "Ft";
	}
    }

    setAxisScale(yLeft, 0.0, ymax * 1.1);
    setAxisTitle(yLeft, ylabel);

    enableAxis(yRight, speedCurve->isVisible());
    setAxisTitle(yRight, (useMetricUnits ? "KPH" : "MPH"));
}

void
AllPlot::setXTitle()
{
    if (bydist)
        setAxisTitle(xBottom, "Distance "+QString(unit.toString() == "Metric"?"(km)":"(miles)"));
    else
        setAxisTitle(xBottom, "Time (minutes)");
}

void
AllPlot::setData(RideItem *_rideItem)
{
    rideItem = _rideItem;

    wattsArray.clear();

    RideFile *ride = rideItem->ride;
    if (ride) {
	setTitle(ride->startTime().toString(GC_DATETIME_FORMAT));

	RideFileDataPresent *dataPresent = ride->areDataPresent();
	int npoints = ride->dataPoints().size();
	wattsArray.resize(dataPresent->watts ? npoints : 0);
	hrArray.resize(dataPresent->hr ? npoints : 0);
	speedArray.resize(dataPresent->kph ? npoints : 0);
	cadArray.resize(dataPresent->cad ? npoints : 0);
	altArray.resize(dataPresent->alt ? npoints : 0);
	timeArray.resize(npoints);
	interArray.resize(npoints);
	distanceArray.resize(npoints);

	// attach appropriate curves
	wattsCurve->detach();
	hrCurve->detach();
	speedCurve->detach();
	cadCurve->detach();
	altCurve->detach();
	if (!wattsArray.empty()) wattsCurve->attach(this);
	if (!hrArray.empty()) hrCurve->attach(this);
	if (!speedArray.empty()) speedCurve->attach(this);
	if (!cadArray.empty()) cadCurve->attach(this);
	if (!altArray.empty()) altCurve->attach(this);

	arrayLength = 0;
	QListIterator<RideFilePoint*> i(ride->dataPoints()); 
	while (i.hasNext()) {
	    RideFilePoint *point = i.next();
	    timeArray[arrayLength]  = point->secs;
	    if (!wattsArray.empty())
		wattsArray[arrayLength] = max(0, point->watts);
	    if (!hrArray.empty())
		hrArray[arrayLength]    = max(0, point->hr);
	    if (!speedArray.empty())
		speedArray[arrayLength] = max(0, 
					      (useMetricUnits
					       ? point->kph
					       : point->kph * MILES_PER_KM));
	    if (!cadArray.empty())
		cadArray[arrayLength]   = max(0, point->cad);
            if (!altArray.empty())
                altArray[arrayLength]   = max(0,
                                              (useMetricUnits
                                               ? point->alt
                                               : point->alt * FEET_PER_M));
	    interArray[arrayLength] = point->interval;
	    distanceArray[arrayLength] = max(0,
					     (useMetricUnits
					      ? point->km
					      : point->km * MILES_PER_KM));
	    ++arrayLength;
	}

	recalc();
    }
    else {
	setTitle("no data");
	wattsCurve->detach();
	hrCurve->detach();
	speedCurve->detach();
	cadCurve->detach();
	altCurve->detach();
    }
}

void
AllPlot::showPower(int id) 
{
    wattsCurve->setVisible(id < 2);
    shade_zones = (id == 0);
    setYMax();
    recalc();
}

void
AllPlot::showHr(int state) 
{
    assert(state != Qt::PartiallyChecked);
    hrCurve->setVisible(state == Qt::Checked);
    setYMax();
    replot();
}

void
AllPlot::showSpeed(int state) 
{
    assert(state != Qt::PartiallyChecked);
    speedCurve->setVisible(state == Qt::Checked);
    setYMax();
    replot();
}

void
AllPlot::showCad(int state) 
{
    assert(state != Qt::PartiallyChecked);
    cadCurve->setVisible(state == Qt::Checked);
    setYMax();
    replot();
}

void
AllPlot::showAlt(int state) 
{
    assert(state != Qt::PartiallyChecked);
    altCurve->setVisible(state == Qt::Checked);
    setYMax();
    replot();
}

void
AllPlot::showGrid(int state) 
{
    assert(state != Qt::PartiallyChecked);
    grid->setVisible(state == Qt::Checked);
    replot();
}

void
AllPlot::setSmoothing(int value)
{
    smooth = value;
    recalc();
}

void
AllPlot::setByDistance(int id)
{
    bydist = (id == 1);
    setXTitle();
    recalc();
}
