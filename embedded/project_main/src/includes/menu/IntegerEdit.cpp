/*
 * IntegerEdit.cpp
 *
 *  Created on: 2.2.2016
 *      Author: krl
 */

#include "IntegerEdit.h"
#include <cstdio>

IntegerEdit::IntegerEdit(LiquidCrystal *lcd_, std::string editTitle, int upper, int lower, int step, bool adjustable):
lcd(lcd_), title(editTitle), upper_limit(upper), lower_limit(lower), step_size(step), adjustable(adjustable)
{
	value = 0;
	edit = 0;
	focus = false;
}

IntegerEdit::~IntegerEdit() {
}

void IntegerEdit::increment() {
	if (!adjustable) return;

	edit += step_size;
	if (edit > upper_limit) edit = 0;
}

void IntegerEdit::decrement() {
	if (!adjustable) return;

	edit -= step_size;
	if (edit < lower_limit) edit = upper_limit;
}

void IntegerEdit::accept() {
	save();
}

void IntegerEdit::cancel() {
	edit = value;
}


void IntegerEdit::setFocus(bool focus) {
	this->focus = focus;
}

bool IntegerEdit::getFocus() {
	return this->focus;
}

void IntegerEdit::display() {
	lcd->clear();
	lcd->print(title);
	char s[17];
	if(focus) {
		snprintf(s, 17, "     [%4d]     ", edit);
	}
	else {
		snprintf(s, 17, "      %4d      ", edit);
	}
	lcd->print(s, 16);
}


void IntegerEdit::save() {
	// set current value to be same as edit value
	value = edit;
	// todo: save current value for example to EEPROM for permanent storage
}

int IntegerEdit::getValue() {
	return value;
}

void IntegerEdit::setValue(int value) {
	edit = value;
	save();
}

void IntegerEdit::toggle_adjust()
{
	adjustable = !adjustable;
}
